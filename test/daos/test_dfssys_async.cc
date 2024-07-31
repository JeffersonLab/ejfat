/**
    Baisc DAOS system asynchronous write test.

    First checked in by xmei@jlab.org on Aug/1/2024.
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
#define EJFAT_DFS_SYS_FILE_PATH "/ejfat-data/async.dat"
/// NOTE: permission and mode for creating a file.
#define EJFAT_DFS_FILE_PERMISSION S_IFREG | EJFAT_DFS_DIR_PERMISSION

#define NUM_WRITE_FILE 5

/**
 * Open a dir indicated by @param _dir_path. @return return code (0 for Success).
*/
int openAndCreateDir_duplicate (dfs_sys_t *_dfs_sys, const char *_dir_path) {
    DIR *_dir;
    int rc = dfs_sys_opendir(_dfs_sys, _dir_path, O_RDWR, &_dir);
    if (rc != 0) {
        rc = dfs_sys_mkdir(_dfs_sys, _dir_path, EJFAT_DFS_DIR_PERMISSION,
        /* daos_oclass_id_t cid, 0 for default MAX_RW */ 0);
    }
    // std::cout << "Open and create [" << _dir_path << "] return: " << rc << std::endl;
    return rc;
}


/**
 * Write "Hello, DAOS!" several times to a newly created file via dfs_sys_write
 *  in asynchronous mode.
 * DAOS event queue is a must here.
*/
TEST(AsycTest, DfsSys_WriteFile_Async) {
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
    ASSERT_EQ(openAndCreateDir_duplicate(mnt_fs_sys, EJFAT_DFS_SYS_DIR_PATH), 0);

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
    daos_size_t wrt_size_single = strlen(wrt_buf);
    daos_size_t wrt_size_all = wrt_size_single * NUM_WRITE_FILE;

    // Create the event queue
    daos_handle_t eq;
    ASSERT_EQ(daos_eq_create(&eq), 0);
    // Initialize events
    daos_event_t events[NUM_WRITE_FILE]; // Array of DAOS event
    for (int i = 0; i < NUM_WRITE_FILE; ++i) {
        ASSERT_EQ(daos_event_init(&events[i], eq, NULL), 0);
    }

    int rc = -1;
    for (int i = 0; i < NUM_WRITE_FILE; ++i) {
        rc = dfs_sys_write(mnt_fs_sys, _dfs_obj, wrt_buf,
        /* Write offset. */ i * wrt_size_single, &wrt_size_single, &events[i]);
        // std::cout << "Write no: " << i << ", write_size=" << wrt_size_single << std::endl;
        EXPECT_EQ(rc, 0);
    }

    // Wait for all the events in event queue finished
    daos_event_t *evp[NUM_WRITE_FILE];
    int num_ev = daos_eq_poll(eq, 1, DAOS_EQ_WAIT, NUM_WRITE_FILE, evp);
    // std::cout << "ready events num: " << num_ev << std::endl;
    ASSERT_EQ(num_ev, NUM_WRITE_FILE);  // asssert all 5 events are finished.
    for (int i=0; i < NUM_WRITE_FILE; i++) {
        ASSERT_EQ(evp[i]->ev_error, 0);
    }

    // Release all events
	for (int i = 0; i < NUM_WRITE_FILE; i++) {
		ASSERT_EQ(daos_event_fini(&events[i]), 0);
	}

	/**
     * Read from the file.
    */
	char read_buf[wrt_size_all];
    memset(read_buf, 0, wrt_size_all);
    // std::cout << "read_buf [init]: " << read_buf << std::endl;

    // Read first 5 bytes.
    daos_size_t got_size = 5;
    ASSERT_EQ(dfs_sys_read(mnt_fs_sys, _dfs_obj, read_buf, 0, &got_size, NULL), 0);
    // std::cout << "read_buf: " << read_buf << std::endl;
    EXPECT_STREQ("Hello", read_buf);

    // Read the whole buffer.
    got_size = wrt_size_all;
    memset(read_buf, 0, wrt_size_all);
    std::cout << "Read_buf [reset]: " << read_buf << std::endl;
	ASSERT_EQ(dfs_sys_read(mnt_fs_sys, _dfs_obj, read_buf, 0, &got_size, NULL), 0);
	std::cout << "Read buf: " << read_buf << std::endl;

    /* Close file object*/
    ASSERT_EQ(dfs_sys_close(_dfs_obj), 0);

    /** destroy event queue */
	ASSERT_EQ(daos_eq_destroy(eq, 0), 0);

    ASSERT_EQ(dfs_sys_umount(mnt_fs_sys), 0);
    ASSERT_EQ(daos_cont_close(coh, NULL), 0);
    ASSERT_EQ(daos_pool_disconnect(poh, NULL), 0);

    ASSERT_EQ(daos_fini(), 0);
}
