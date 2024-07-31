/**
    Baisc DAOS system filesystem (DFS, POSIX APIs) test.

    First checked in by xmei@jlab.org on May/16/2024.
*/

#include <gtest/gtest.h>
#include <daos.h>
#include <daos_fs_sys.h>

#include <iostream>
#include <fcntl.h> // for O_XXX values

/// NOTE: libdfs is a standalone library. It's not a part of libdaos.

/**
 * NOTE: dfs_sys.h unit tests example:
 * https://github.com/daos-stack/daos/blob/v2.4.0/src/tests/suite/dfs_sys_unit_test.c
 *
 * NOTE: when finishing this test, there should be 2 objects in the container.
 * Check with `daos cont list-obj <pool_label> <cont_label>.
 */

// Make sure this pool exists. Query with `dmg sys query list-pools`.
#define EJFAT_DAOS_POOL_LABEL "sc"

// Create this container with parameter "--type=POSIX". Otherwise the test will fail.
// Example: "daos create cont <pool_label> <cont_label> --type=POSIX"
// Container type ref: https://docs.daos.io/v2.4/user/container/#container-type
#define EJFAT_DAOS_CONT_LABEL "fs"

#define EJFAT_DFS_SYS_DIR_PATH "/ejfat-data"
// Dir RW by the user and group
#define EJFAT_DFS_DIR_PERMISSION S_IWUSR | S_IRUSR | S_IRGRP | S_IWGRP

// FILE path should include DIR path.
#define EJFAT_DFS_SYS_FILE_PATH "/ejfat-data/file.dat"
/// NOTE: permission and mode for creating a file.
#define EJFAT_DFS_FILE_PERMISSION S_IFREG | EJFAT_DFS_DIR_PERMISSION

void pauseTest() {
    std::cout << "Press any key to continue..." << std::endl;
    std::cin.get();
}

/**
 * Open a dir indicated by @param _dir_path. @return return code (0 for Success).
*/
int openAndCreateDir (dfs_sys_t *_dfs_sys, const char *_dir_path) {
    DIR *_dir;
    int rt = dfs_sys_opendir(_dfs_sys, _dir_path, O_RDWR, &_dir);
    if (rt != 0) {
        rt = dfs_sys_mkdir(_dfs_sys, _dir_path, EJFAT_DFS_DIR_PERMISSION,
        /* daos_oclass_id_t cid, 0 for default MAX_RW */ 0);
    }
    // std::cout << "Open and create [" << _dir_path << "] return: " << rt << std::endl;
    return rt;
}

/**
 * Mount a DFS userspace to an existing POSIX DAOS container.
 * Open the POSIX container in RO mode.
*/
TEST(DfsSysTest, DfsSys_Mount) {

    ASSERT_EQ(daos_init(), 0);

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    // Get the pool and container handles.
    daos_handle_t poh = DAOS_HDL_INVAL, coh = DAOS_HDL_INVAL;
    ASSERT_EQ(daos_pool_connect(pool_label, NULL, DAOS_PC_RO, &poh, NULL, NULL), 0);
    ASSERT_EQ(daos_cont_open(poh, cont_label, DAOS_COO_RO, &coh, NULL, NULL), 0);

    // Mount the userspace to a POSIX container.
    dfs_sys_t *mnt_fs_sys = nullptr;
    EXPECT_EQ(dfs_sys_mount(poh, coh, O_RDONLY, DFS_SYS_NO_CACHE, &mnt_fs_sys), 0);
    EXPECT_NE(mnt_fs_sys, nullptr);

    dfs_t *test_fs = nullptr;
    EXPECT_EQ(dfs_sys2base(mnt_fs_sys, &test_fs), 0);

    /**
     * Compare the mounted container cookie with the assigned one.
     * By doing this we make sure the mounting is successful.
     */
    daos_handle_t res_coh = DAOS_HDL_INVAL;
    EXPECT_EQ(dfs_cont_get(test_fs, &res_coh), 0);
    EXPECT_EQ(coh.cookie, res_coh.cookie);

    /// NOTE: Must-have after calling dfs_cont_put, otherwise DFS cannot be unmounted.
    EXPECT_EQ(dfs_cont_put(test_fs, res_coh), 0);

    EXPECT_EQ(dfs_sys_umount(mnt_fs_sys), 0);

    ASSERT_EQ(daos_cont_close(coh, NULL), 0);
    ASSERT_EQ(daos_pool_disconnect(poh, NULL), 0);
    ASSERT_EQ(daos_fini(), 0);
}

/**
 * Make a directory using the RW mode.
*/
TEST(DfsSysTest, DfsSys_MkdirAndDel) {

    ASSERT_EQ(daos_init(), 0);

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    // Get the pool and container handles. Remeber to use RW mode.
    daos_handle_t poh = DAOS_HDL_INVAL, coh = DAOS_HDL_INVAL;
    ASSERT_EQ(daos_pool_connect(pool_label, NULL, DAOS_PC_RW, &poh, NULL, NULL), 0);
    ASSERT_EQ(daos_cont_open(poh, cont_label, DAOS_COO_RW, &coh, NULL, NULL), 0);

    // Mount the userspace to a POSIX container. Mode must be RW.
    dfs_sys_t *mnt_fs_sys = nullptr;
    ASSERT_EQ(dfs_sys_mount(poh, coh, O_RDWR, DFS_SYS_NO_CACHE, &mnt_fs_sys), 0);
    ASSERT_NE(mnt_fs_sys, nullptr);

    // Create a directory. If this directory exits, open it.
    /// NOTE: If we mount the container with `dfuse`, we are able to ls this directory.
    EXPECT_EQ(openAndCreateDir(mnt_fs_sys, EJFAT_DFS_SYS_DIR_PATH), 0);

    /** 
     * NOTE: pause here to check whether the dir is created at the mounting place.
     * Mount with: dfuse <mnt_space> --pool=<pool_label> --cont=<cont_label>
     * Check by: ls <mnt_space>
     * Should see a dir named by @var EJFAT_DFS_SYS_DIR_PATH is created.
    */
    // pauseTest();

    struct stat mnt_stat;
    ASSERT_EQ(dfs_sys_stat(mnt_fs_sys, EJFAT_DFS_SYS_DIR_PATH, O_NOFOLLOW, &mnt_stat), 0);
    // std::cout << "stat.st_uid: " <<q_stat.st_uid << std::endl;
    // std::cout << "stat.st_gid: " <<q_stat.st_gid << std::endl;  // `id <user>` to confirm uid and gid 
    // std::cout << "stat.st_size: " << q_stat.st_size << std::endl;  // in bytes
    // std::cout << "stat.st_blocks: " << q_stat.st_blocks << std::endl;  // number of 512-byte blocks
    // std::cout << "q_stat.st_blksize: " << q_stat.st_blksize << std::endl;
    ASSERT_NE(mnt_stat.st_size, 0);

    // Delete the directory if we want to mkdir again in the future.
    daos_obj_id_t del_oid = DAOS_OBJ_NIL;
    ASSERT_EQ(dfs_sys_remove(mnt_fs_sys, EJFAT_DFS_SYS_DIR_PATH,
        /* force mode */ true, &del_oid), 0);
    // std::cout << "Delete object << " << del_oid.hi << "." << del_oid.lo << std::endl;
    ASSERT_NE(del_oid.hi, 0);

    ASSERT_EQ(dfs_sys_umount(mnt_fs_sys), 0);

    ASSERT_EQ(daos_cont_close(coh, NULL), 0);
    ASSERT_EQ(daos_pool_disconnect(poh, NULL), 0);
    
    ASSERT_EQ(daos_fini(), 0);
}


/**
 * Write "Hello, DAOS!" to a newly created file.
 * Verify can read the same content and then delete it.
*/
TEST(DfsSysTest, DfsSys_WriteFile) {
    ASSERT_EQ(daos_init(), 0);

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    // Get the pool and container handles. Remeber to use RW mode.
    daos_handle_t poh = DAOS_HDL_INVAL, coh = DAOS_HDL_INVAL;
    ASSERT_EQ(daos_pool_connect(pool_label, NULL, DAOS_PC_RW, &poh, NULL, NULL), 0);
    ASSERT_EQ(daos_cont_open(poh, cont_label, DAOS_COO_RW, &coh, NULL, NULL), 0);

    // Mount the userspace to a POSIX container. Mode must be RW.
    dfs_sys_t *mnt_fs_sys = nullptr;
    ASSERT_EQ(dfs_sys_mount(poh, coh, O_RDWR, DFS_SYS_NO_CACHE, &mnt_fs_sys), 0);
    ASSERT_NE(mnt_fs_sys, nullptr);

    /**
     * Create the file's parent dir.
    */
    ASSERT_EQ(openAndCreateDir(mnt_fs_sys, EJFAT_DFS_SYS_DIR_PATH), 0);

    /**
     * Create the file.
    */
	ASSERT_EQ(dfs_sys_mknod(mnt_fs_sys, EJFAT_DFS_SYS_FILE_PATH,
        /* permission and type */ EJFAT_DFS_FILE_PERMISSION, 0, 0), 0);
    // Check whether we have RW permission.
    ASSERT_EQ(dfs_sys_access(mnt_fs_sys, EJFAT_DFS_SYS_FILE_PATH, R_OK | W_OK, 0), 0);
    
    /**
     * Open a file object and write to the file.
    */
    dfs_obj_t *_dfs_obj = nullptr;
    ASSERT_EQ(dfs_sys_open(mnt_fs_sys, EJFAT_DFS_SYS_FILE_PATH,
        EJFAT_DFS_FILE_PERMISSION,
		O_RDWR,
        0, 0, NULL, &_dfs_obj), 0);
    ASSERT_NE(_dfs_obj, nullptr);
    
    const char *wrt_buf = "Hello, DAOS!";
    daos_size_t wrt_size = strlen(wrt_buf);

	ASSERT_EQ(dfs_sys_write(mnt_fs_sys, _dfs_obj, wrt_buf,
        /* Write offset. */ 0, &wrt_size, NULL), 0);

    /// NOTE: pause to check if the file is created.    
    // pauseTest();

	/** 
     * Read from the file. 
    */
	char read_buf[wrt_size];
    memset(read_buf, 0, wrt_size);
    // std::cout << "read_buf [init]: " << read_buf << std::endl;

    // Read first 5 bytes.
    daos_size_t got_size = 5;
    ASSERT_EQ(dfs_sys_read(mnt_fs_sys, _dfs_obj, read_buf, 0, &got_size, NULL), 0);
    // std::cout << "read_buf: " << read_buf << std::endl;
    EXPECT_STREQ("Hello", read_buf);
    
    // Read the whole buffer.
    got_size = wrt_size;
    memset(read_buf, 0, wrt_size);
    // std::cout << "read_buf [reset]: " << read_buf << std::endl;
	ASSERT_EQ(dfs_sys_read(mnt_fs_sys, _dfs_obj, read_buf, 0, &got_size, NULL), 0);
    // std::cout << "read_buf: " << read_buf << std::endl;
	EXPECT_STREQ(read_buf, wrt_buf);  // verify we get the same string

	/* Punch file. Namely delete some contents in the file. */
	ASSERT_EQ(dfs_sys_punch(mnt_fs_sys, EJFAT_DFS_SYS_FILE_PATH, 0, wrt_size), 0);

    /* Read empty file. */
    memset(read_buf, 0, wrt_size);
    ASSERT_EQ(got_size, wrt_size);
    ASSERT_EQ(dfs_sys_read(mnt_fs_sys, _dfs_obj, read_buf, 0, &got_size, NULL), 0);
    ASSERT_EQ(got_size, 0);  // make sure the read-out length is 0.
    
    /* Close file object*/
    ASSERT_EQ(dfs_sys_close(_dfs_obj), 0);

    /* Delete the file */
    ASSERT_EQ(dfs_sys_remove(mnt_fs_sys, EJFAT_DFS_SYS_FILE_PATH, true, 0), 0);

    /// NOTE: pause to check if the file is deleted.    
    // pauseTest();

    // Delete the directory.

    ASSERT_EQ(dfs_sys_remove(mnt_fs_sys, EJFAT_DFS_SYS_DIR_PATH, true, 0), 0);

    ASSERT_EQ(dfs_sys_umount(mnt_fs_sys), 0);
    ASSERT_EQ(daos_cont_close(coh, NULL), 0);
    ASSERT_EQ(daos_pool_disconnect(poh, NULL), 0);

    ASSERT_EQ(daos_fini(), 0);
}
