/**
 * Basic DAOS pool/container connections.
 * 
 * First checked in by xmei@jlab.org on Mar/26/2024.
*/

// DAOS storage model README:
// https://github.com/daos-stack/daos/blob/v2.4.0/docs/overview/storage.md
// DAOS include headers:
// https://github.com/daos-stack/daos/tree/v2.4.0/src/include

#include <gtest/gtest.h>
#include <daos.h>

// Make sure this pool exists. Query with `dmg sys query list-pools`.
#define EJFAT_DAOS_POOL_LABEL "sc"

// Make sure this container exists. List the conts in a DAOS pool by `daos cont ls <pool_label>`. 
#define EJFAT_DAOS_CONT_LABEL "cont1"

/**
 * Connect/disconnect from an exisiting DAOS pool/container.
 * The container must reside in the pool. 
*/
TEST(DaosTest, ContainerReadOnly) {

    // Must init before any daos calls.
    // If doesnot init, return value of the following pool/cont calls will be -1015.
    // "DER_UNINIT(-1015): Device or resource not initialized"
    daos_init();

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    daos_handle_t poh = DAOS_HDL_INVAL;  // init with value {0}
    daos_handle_t coh = DAOS_HDL_INVAL;

    // Connect to an existing pool with Read-only mode.
    EXPECT_EQ(daos_pool_connect(pool_label,
        /* sys */ NULL,
        /* read-only mode */ DAOS_PC_RO,
        &poh,
        /* daos_pool_info_t *, optional, param [in, out] */ NULL,
        /* daos_event_t *, optional, NULL means running in blocking mode */ NULL), 0);

    // Open an existing container with Read-only mode.
    EXPECT_EQ(daos_cont_open(poh, cont_label, DAOS_COO_RO, &coh, NULL, NULL), 0);
    // Close the container.
    EXPECT_EQ(daos_cont_close(coh, NULL), 0);
    // Disconnect from the pool.
    EXPECT_EQ(daos_pool_disconnect(poh, NULL), 0);

    // Finalize the DAOS call.
    daos_fini();
}
