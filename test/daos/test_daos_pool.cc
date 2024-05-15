/**
 * DAOS pool APIs.
 * 
 * First checked in by xmei@jlab.org on April/17/2024.
*/

// DAOS storage model README:
// https://github.com/daos-stack/daos/blob/v2.4.0/docs/overview/storage.md
// DAOS include headers:
// https://github.com/daos-stack/daos/tree/v2.4.0/src/include

#include <gtest/gtest.h>
#include <daos.h>
#include <iostream>

// Make sure this pool exists. Query with `dmg sys query list-pools`.
#define EJFAT_DAOS_POOL_LABEL "ejfat"

// Make sure this container exists. List the conts in a DAOS pool by `daos cont ls <pool_label>`. 
#define EJFAT_DAOS_CONT_LABEL "cont1"

/**
 * Connect/disconnect from an exisiting DAOS pool/container.
 * The container must reside in the pool. 
*/
TEST(DaosTest, QueryPoolUsage) {

    // Must init before any daos calls.
    // If doesnot init, return value of the following pool/cont calls will be -1015.
    // "DER_UNINIT(-1015): Device or resource not initialized"
    daos_init();

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    daos_handle_t poh = DAOS_HDL_INVAL;  // init with value {0}
    daos_handle_t coh = DAOS_HDL_INVAL;

    // Connect to an existing pool with Read-only mode.
    ASSERT_EQ(daos_pool_connect(pool_label,
        /* sys */ NULL,
        /* read-only mode */ DAOS_PC_RO,
        &poh,
        /* daos_pool_info_t *, optional, param [in, out] */ NULL,
        /* daos_event_t *, optional, NULL means running in blocking mode */ NULL), 0);

    // Query the pool usage
    daos_pool_info_t pool_info = {0};
    pool_info.pi_bits = DPI_SPACE;  // IMPORTANT!!! Set daos_pool_info_bit (pi_bits) to 1.
    ASSERT_EQ(daos_pool_query(poh, NULL, &pool_info, NULL, NULL), 0);

    // pool_info.pi_space.ps_space.s_xx[0] is for tier 0 (SCM)
    // pool_info.pi_space.ps_space.s_xx[1] is for tier 1 (NVME)
    // std::cout << "Pool total bytes [0]:  " << pool_info.pi_space.ps_space.s_total[0] << std::endl;
    // std::cout << "Pool total bytes [1]:  " << pool_info.pi_space.ps_space.s_total[1] << std::endl;

    // std::cout << "Pool free bytes [0]:  " << pool_info.pi_space.ps_space.s_free[0] << std::endl;
    // std::cout << "Pool free bytes [1]:  " << pool_info.pi_space.ps_space.s_free[1] << std::endl;

    // std::cout << "Pool usage [0]:  " << \
    //     100.0 - pool_info.pi_space.ps_space.s_free[0] * 100.0 / pool_info.pi_space.ps_space.s_total[0] << std::endl;
    // std::cout << "Pool usage [1]:  " << \
    //     100.0 - pool_info.pi_space.ps_space.s_free[1] * 100.0 / pool_info.pi_space.ps_space.s_total[1] << std::endl;

    ASSERT_GT(pool_info.pi_space.ps_space.s_total[0], pool_info.pi_space.ps_space.s_free[0]);
    ASSERT_GT(pool_info.pi_space.ps_space.s_total[1], pool_info.pi_space.ps_space.s_free[1]);

    // Disconnect from the pool.
    ASSERT_EQ(daos_pool_disconnect(poh, NULL), 0);

    // Finalize the DAOS call.
    daos_fini();
}
