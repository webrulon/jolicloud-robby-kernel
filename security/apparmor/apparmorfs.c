/*
 * AppArmor security module
 *
 * This file contains AppArmor /sys/kernel/security/apparmor interface functions
 *
 * Copyright (C) 1998-2008 Novell/SUSE
 * Copyright 2009-2010 Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 */

#include <linux/security.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/namei.h>

#include "include/apparmor.h"
#include "include/apparmorfs.h"
#include "include/audit.h"
#include "include/context.h"
#include "include/policy.h"

/**
 * kvmalloc - do allocation prefering kmalloc but falling back to vmalloc
 * @size: size of allocation
 *
 * Return: allocated buffer or NULL if failed
 *
 * It is possible that policy being loaded from the user is larger than
 * what can be allocated by kmalloc, in those cases fall back to vmalloc.
 */
static void *kvmalloc(size_t size)
{
	void *buffer;

	if (size == 0)
		return NULL;

	buffer = kmalloc(size, GFP_KERNEL | __GFP_NOWARN);
	if (!buffer)
		buffer = vmalloc(size);
	return buffer;
}

/**
 * kvfree - free an allocation do by kvmalloc
 * @buffer: buffer to free
 *
 * Free a buffer allocated by kvmalloc
 */
static void kvfree(void *buffer)
{
	if (!buffer)
		return;

	if (is_vmalloc_addr(buffer))
		vfree(buffer);
	else
		kfree(buffer);
}

/**
 * aa_simple_write_to_buffer - common routine for getting policy from user
 * @op: operation doing the user buffer copy
 * @userbuf: user buffer to copy data from  (NOT NULL)
 * @alloc_size: size of user buffer
 * @copy_size: size of data to copy from user buffer
 * @pos: position write is at in the file
 *
 * Returns: kernel buffer containing copy of user buffer data or an
 *          ERR_PTR on failure.
 */
static char *aa_simple_write_to_buffer(int op, const char __user *userbuf,
				       size_t alloc_size, size_t copy_size,
				       loff_t *pos)
{
	char *data;

	if (*pos != 0) {
		/* only writes from pos 0, that is complete writes */
		data = ERR_PTR(-ESPIPE);
		goto out;
	}

	/*
	 * Don't allow profile load/replace/remove from profiles that don't
	 * have CAP_MAC_ADMIN
	 */
	if (!aa_may_manage_policy(op))
		return ERR_PTR(-EACCES);

	/* freed by caller to simple_write_to_buffer */
	data = kvmalloc(alloc_size);
	if (data == NULL)
		return ERR_PTR(-ENOMEM);

	if (copy_from_user(data, userbuf, copy_size)) {
		kvfree(data);
		data = ERR_PTR(-EFAULT);
		goto out;
	}

out:
	return data;
}


/* .load file hook fn to load policy */
static ssize_t profile_load(struct file *f, const char __user *buf, size_t size,
			    loff_t *pos)
{
	char *data;
	ssize_t error;

	data = aa_simple_write_to_buffer(OP_PROF_LOAD, buf, size, size, pos);

	error = PTR_ERR(data);
	if (!IS_ERR(data)) {
		error = aa_replace_profiles(data, size, PROF_ADD);
		kvfree(data);
	}

	return error;
}

static const struct file_operations aa_fs_profile_load = {
	.write = profile_load
};

/* .replace file hook fn to load and/or replace policy */
static ssize_t profile_replace(struct file *f, const char __user *buf,
			       size_t size, loff_t *pos)
{
	char *data;
	ssize_t error;

	data = aa_simple_write_to_buffer(OP_PROF_REPL, buf, size, size, pos);
	error = PTR_ERR(data);
	if (!IS_ERR(data)) {
		error = aa_replace_profiles(data, size, PROF_REPLACE);
		kvfree(data);
	}

	return error;
}

static const struct file_operations aa_fs_profile_replace = {
	.write = profile_replace
};

/* .remove file hook fn to remove loaded policy */
static ssize_t profile_remove(struct file *f, const char __user *buf,
			      size_t size, loff_t *pos)
{
	char *data;
	ssize_t error;

	/*
	 * aa_remove_profile needs a null terminated string so 1 extra
	 * byte is allocated and the copied data is null terminated.
	 */
	data = aa_simple_write_to_buffer(OP_PROF_RM, buf, size + 1, size, pos);

	error = PTR_ERR(data);
	if (!IS_ERR(data)) {
		data[size] = 0;
		error = aa_remove_profiles(data, size);
		kvfree(data);
	}

	return error;
}

static const struct file_operations aa_fs_profile_remove = {
	.write = profile_remove
};

/** Base file system setup **/

static struct dentry *aa_fs_dentry;
struct dentry *aa_fs_null;
struct vfsmount *aa_fs_mnt;

static void aafs_remove(const char *name)
{
	struct dentry *dentry;

	dentry = lookup_one_len(name, aa_fs_dentry, strlen(name));
	if (!IS_ERR(dentry)) {
		securityfs_remove(dentry);
		dput(dentry);
	}
}

/**
 * aafs_create - create an entry in the apparmor filesystem
 * @name: name of the entry
 * @mask: file permission mask of the file
 * @fops: file operations for the file
 *
 * Used aafs_remove to remove entries created with this fn.
 */
static int aafs_create(const char *name, int mask,
		       const struct file_operations *fops)
{
	struct dentry *dentry;

	dentry = securityfs_create_file(name, S_IFREG | mask, aa_fs_dentry,
					NULL, fops);

	return IS_ERR(dentry) ? PTR_ERR(dentry) : 0;
}

/**
 * aa_destroy_aafs - cleanup and free aafs
 *
 * releases dentries allocated by aa_create_aafs
 */
void aa_destroy_aafs(void)
{
	if (aa_fs_dentry) {
		aafs_remove(".remove");
		aafs_remove(".replace");
		aafs_remove(".load");
		aafs_remove("profiles");
#ifdef CONFIG_SECURITY_APPARMOR_COMPAT_24
		aafs_remove("matching");
		aafs_remove("features");
#endif
		securityfs_remove(aa_fs_dentry);
		aa_fs_dentry = NULL;
	}
}

/**
 * aa_create_aafs - create the apparmor security filesystem
 *
 * dentries created here are released by aa_destroy_aafs
 *
 * Returns: error on failure
 */
int aa_create_aafs(void)
{
	int error;

	if (!apparmor_initialized)
		return 0;

	if (aa_fs_dentry) {
		AA_ERROR("%s: AppArmor securityfs already exists\n", __func__);
		return -EEXIST;
	}

	aa_fs_dentry = securityfs_create_dir("apparmor", NULL);
	if (IS_ERR(aa_fs_dentry)) {
		error = PTR_ERR(aa_fs_dentry);
		aa_fs_dentry = NULL;
		goto error;
	}
#ifdef CONFIG_SECURITY_APPARMOR_COMPAT_24
	error = aafs_create("matching", 0444, &aa_fs_matching_fops);
	if (error)
		goto error;
	error = aafs_create("features", 0444, &aa_fs_features_fops);
	if (error)
		goto error;
#endif
	error = aafs_create("profiles", 0440, &aa_fs_profiles_fops);
	if (error)
		goto error;
	error = aafs_create(".load", 0640, &aa_fs_profile_load);
	if (error)
		goto error;
	error = aafs_create(".replace", 0640, &aa_fs_profile_replace);
	if (error)
		goto error;
	error = aafs_create(".remove", 0640, &aa_fs_profile_remove);
	if (error)
		goto error;

	/* TODO: add support for apparmorfs_null and apparmorfs_mnt */

	/* Report that AppArmor fs is enabled */
	aa_info_message("AppArmor Filesystem Enabled");
	return 0;

error:
	aa_destroy_aafs();
	AA_ERROR("Error creating AppArmor securityfs\n");
	return error;
}

fs_initcall(aa_create_aafs);