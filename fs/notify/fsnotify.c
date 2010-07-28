/*
 *  Copyright (C) 2008 Red Hat, Inc., Eric Paris <eparis@redhat.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/srcu.h>

#include <linux/fsnotify_backend.h>
#include "fsnotify.h"

/*
 * Clear all of the marks on an inode when it is being evicted from core
 */
void __fsnotify_inode_delete(struct inode *inode)
{
	fsnotify_clear_marks_by_inode(inode);
}
EXPORT_SYMBOL_GPL(__fsnotify_inode_delete);

void __fsnotify_vfsmount_delete(struct vfsmount *mnt)
{
	fsnotify_clear_marks_by_mount(mnt);
}

/*
 * Given an inode, first check if we care what happens to our children.  Inotify
 * and dnotify both tell their parents about events.  If we care about any event
 * on a child we run all of our children and set a dentry flag saying that the
 * parent cares.  Thus when an event happens on a child it can quickly tell if
 * if there is a need to find a parent and send the event to the parent.
 */
void __fsnotify_update_child_dentry_flags(struct inode *inode)
{
	struct dentry *alias;
	int watched;

	if (!S_ISDIR(inode->i_mode))
		return;

	/* determine if the children should tell inode about their events */
	watched = fsnotify_inode_watches_children(inode);

	spin_lock(&dcache_lock);
	/* run all of the dentries associated with this inode.  Since this is a
	 * directory, there damn well better only be one item on this list */
	list_for_each_entry(alias, &inode->i_dentry, d_alias) {
		struct dentry *child;

		/* run all of the children of the original inode and fix their
		 * d_flags to indicate parental interest (their parent is the
		 * original inode) */
		list_for_each_entry(child, &alias->d_subdirs, d_u.d_child) {
			if (!child->d_inode)
				continue;

			spin_lock(&child->d_lock);
			if (watched)
				child->d_flags |= DCACHE_FSNOTIFY_PARENT_WATCHED;
			else
				child->d_flags &= ~DCACHE_FSNOTIFY_PARENT_WATCHED;
			spin_unlock(&child->d_lock);
		}
	}
	spin_unlock(&dcache_lock);
}

/* Notify this dentry's parent about a child's events. */
void __fsnotify_parent(struct file *file, struct dentry *dentry, __u32 mask)
{
	struct dentry *parent;
	struct inode *p_inode;
	bool send = false;
	bool should_update_children = false;

	if (!dentry)
		dentry = file->f_path.dentry;

	if (!(dentry->d_flags & DCACHE_FSNOTIFY_PARENT_WATCHED))
		return;

	spin_lock(&dentry->d_lock);
	parent = dentry->d_parent;
	p_inode = parent->d_inode;

	if (fsnotify_inode_watches_children(p_inode)) {
		if (p_inode->i_fsnotify_mask & mask) {
			dget(parent);
			send = true;
		}
	} else {
		/*
		 * The parent doesn't care about events on it's children but
		 * at least one child thought it did.  We need to run all the
		 * children and update their d_flags to let them know p_inode
		 * doesn't care about them any more.
		 */
		dget(parent);
		should_update_children = true;
	}

	spin_unlock(&dentry->d_lock);

	if (send) {
		/* we are notifying a parent so come up with the new mask which
		 * specifies these are events which came from a child. */
		mask |= FS_EVENT_ON_CHILD;

		if (file)
			fsnotify(p_inode, mask, file, FSNOTIFY_EVENT_FILE,
				 dentry->d_name.name, 0);
		else
			fsnotify(p_inode, mask, dentry->d_inode, FSNOTIFY_EVENT_INODE,
				 dentry->d_name.name, 0);
		dput(parent);
	}

	if (unlikely(should_update_children)) {
		__fsnotify_update_child_dentry_flags(p_inode);
		dput(parent);
	}
}
EXPORT_SYMBOL_GPL(__fsnotify_parent);

static int send_to_group(struct inode *to_tell, struct vfsmount *mnt,
			 struct fsnotify_mark *mark,
			__u32 mask, void *data,
			 int data_is, u32 cookie,
			 const unsigned char *file_name,
			 struct fsnotify_event **event)
{
	struct fsnotify_group *group = mark->group;
	__u32 test_mask = (mask & ~FS_EVENT_ON_CHILD);

	pr_debug("%s: group=%p to_tell=%p mnt=%p mark=%p mask=%x data=%p"
		 " data_is=%d cookie=%d event=%p\n", __func__, group, to_tell,
		 mnt, mark, mask, data, data_is, cookie, *event);

	if ((mask & FS_MODIFY) &&
	    !(mark->flags & FSNOTIFY_MARK_FLAG_IGNORED_SURV_MODIFY))
		mark->ignored_mask = 0;

	if (!(test_mask & mark->mask & ~mark->ignored_mask))
		return 0;

	if (group->ops->should_send_event(group, to_tell, mnt, mark, mask,
					  data, data_is) == false)
		return 0;

	if (!*event) {
		*event = fsnotify_create_event(to_tell, mask, data,
						data_is, file_name,
						cookie, GFP_KERNEL);
		if (!*event)
			return -ENOMEM;
	}
	return group->ops->handle_event(group, mark, *event);
}

/*
 * This is the main call to fsnotify.  The VFS calls into hook specific functions
 * in linux/fsnotify.h.  Those functions then in turn call here.  Here will call
 * out to all of the registered fsnotify_group.  Those groups can then use the
 * notification event in whatever means they feel necessary.
 */
int fsnotify(struct inode *to_tell, __u32 mask, void *data, int data_is,
	     const unsigned char *file_name, u32 cookie)
{
	struct hlist_node *inode_node, *vfsmount_node;
	struct fsnotify_mark *inode_mark = NULL, *vfsmount_mark = NULL;
	struct fsnotify_group *inode_group, *vfsmount_group;
	struct fsnotify_event *event = NULL;
	struct vfsmount *mnt;
	int idx, ret = 0;
	bool used_inode = false, used_vfsmount = false;
	/* global tests shouldn't care about events on child only the specific event */
	__u32 test_mask = (mask & ~FS_EVENT_ON_CHILD);

	if (data_is == FSNOTIFY_EVENT_FILE)
		mnt = ((struct file *)data)->f_path.mnt;
	else
		mnt = NULL;

	/*
	 * if this is a modify event we may need to clear the ignored masks
	 * otherwise return if neither the inode nor the vfsmount care about
	 * this type of event.
	 */
	if (!(mask & FS_MODIFY) &&
	    !(test_mask & to_tell->i_fsnotify_mask) &&
	    !(mnt && test_mask & mnt->mnt_fsnotify_mask))
		return 0;

	idx = srcu_read_lock(&fsnotify_mark_srcu);

	if ((mask & FS_MODIFY) ||
	    (test_mask & to_tell->i_fsnotify_mask))
		inode_node = to_tell->i_fsnotify_marks.first;
	else
		inode_node = NULL;

	if (mnt) {
		if ((mask & FS_MODIFY) ||
		    (test_mask & mnt->mnt_fsnotify_mask))
			vfsmount_node = mnt->mnt_fsnotify_marks.first;
		else
			vfsmount_node = NULL;
	} else {
		mnt = NULL;
		vfsmount_node = NULL;
	}

	while (inode_node || vfsmount_node) {
		if (inode_node) {
			inode_mark = hlist_entry(srcu_dereference(inode_node, &fsnotify_mark_srcu),
						 struct fsnotify_mark, i.i_list);
			inode_group = inode_mark->group;
		} else
			inode_group = (void *)-1;

		if (vfsmount_node) {
			vfsmount_mark = hlist_entry(srcu_dereference(vfsmount_node, &fsnotify_mark_srcu),
							struct fsnotify_mark, m.m_list);
			vfsmount_group = vfsmount_mark->group;
		} else
			vfsmount_group = (void *)-1;

		if (inode_group < vfsmount_group) {
			/* handle inode */
			send_to_group(to_tell, NULL, inode_mark, mask, data,
				      data_is, cookie, file_name, &event);
			used_inode = true;
		} else if (vfsmount_group < inode_group) {
			send_to_group(to_tell, mnt, vfsmount_mark, mask, data,
				      data_is, cookie, file_name, &event);
			used_vfsmount = true;
		} else {
			send_to_group(to_tell, mnt, vfsmount_mark, mask, data,
				      data_is, cookie, file_name, &event);
			used_vfsmount = true;
			send_to_group(to_tell, NULL, inode_mark, mask, data,
				      data_is, cookie, file_name, &event);
			used_inode = true;
		}

		if (used_inode)
			inode_node = inode_node->next;
		if (used_vfsmount)
			vfsmount_node = vfsmount_node->next;
	}

	srcu_read_unlock(&fsnotify_mark_srcu, idx);
	/*
	 * fsnotify_create_event() took a reference so the event can't be cleaned
	 * up while we are still trying to add it to lists, drop that one.
	 */
	if (event)
		fsnotify_put_event(event);

	return ret;
}
EXPORT_SYMBOL_GPL(fsnotify);

static __init int fsnotify_init(void)
{
	int ret;

	BUG_ON(hweight32(ALL_FSNOTIFY_EVENTS) != 23);

	ret = init_srcu_struct(&fsnotify_mark_srcu);
	if (ret)
		panic("initializing fsnotify_mark_srcu");

	return 0;
}
core_initcall(fsnotify_init);
