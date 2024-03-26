/**
 * DAOS object operation.
 * 
 * First checked in by xmei@jlab.org on Mar/27/2024.
*/

// DAOS object README:
// https://github.com/daos-stack/daos/blob/v2.4.0/src/object/README.md
// DAOS include headers:
// https://github.com/daos-stack/daos/tree/v2.4.0/src/include

#include <gtest/gtest.h>
#include <daos.h>
#include <iostream>  // for some commented std::cout

// Make sure this pool exists. Query with `dmg sys query list-pools`.
#define EJFAT_DAOS_POOL_LABEL "ejfat"

// Make sure this container exists. List the conts in a DAOS pool by `daos cont ls <pool_label>`. 
#define EJFAT_DAOS_CONT_LABEL "cont1"

#define EJFAT_DAOS_OBJECT_ID_LO 1

/**
 * Open a DAOS transaction with Read-only mode.
 * The DAOS pool and container are with RW mode.
*/
TEST(DaosTest, TransactionOpenReadOnly) {

    // Must init before any daos calls.
    // If doesnot init, return value of the following pool/cont calls will be -1015.
    // "DER_UNINIT(-1015): Device or resource not initialized"
    daos_init();

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    daos_handle_t poh, coh, th = DAOS_HDL_INVAL;  // init with value {0}

    EXPECT_EQ(daos_pool_connect(pool_label,
        /* sys */ NULL,
        /* read-and-write mode */ DAOS_PC_RW,
        &poh,
        /* daos_pool_info_t *, optional, param [in, out] */ NULL,
        /* daos_event_t *, optional, NULL means running in blocking mode */ NULL), 0);

    EXPECT_EQ(daos_cont_open(poh, cont_label, DAOS_COO_RW, &coh, NULL, NULL), 0);

    // Start a new transaction. Returns the transaction handle @var th. 
    EXPECT_EQ(daos_tx_open(coh, &th, 
        /* DAOS transaction flags, DAOS_TF_RONLY or DAOS_TF_ZERO_COPY */ DAOS_TF_RDONLY,
        /* runnning in blockingg mode */ NULL), 0);

    // std::cout << "poh: " << poh.cookie << std::endl;
    // std::cout << "coh: " << coh.cookie << std::endl;
    // std::cout << "th: " << th.cookie << std::endl;
    EXPECT_GT(th.cookie, 0);

    EXPECT_EQ(daos_cont_close(coh, NULL), 0);
    EXPECT_EQ(daos_pool_disconnect(poh, NULL), 0);

    daos_fini();
}

/**
 * Create a DAOS object and then punch (delete?) it.
*/
TEST(DaosTest, ObjectCreateAndPunch) {
    daos_init();

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    daos_handle_t poh, coh, oh = DAOS_HDL_INVAL;  // init with value {0}

    EXPECT_EQ(daos_pool_connect(pool_label,
        /* sys */ NULL,
        /* read-and-write mode */ DAOS_PC_RW,
        &poh,
        /* daos_pool_info_t *, optional, param [in, out] */ NULL,
        /* daos_event_t *, optional, NULL means running in blocking mode */ NULL), 0);

    EXPECT_EQ(daos_cont_open(poh, cont_label, DAOS_COO_RW, &coh, NULL, NULL), 0);

    /**
     * DAOS object operation.
     *  There is no C++ object creation API. Refer to the Python one:
     *  https://github.com/daos-stack/daos/blob/115c3d6a22b9009fba3124bc89399551875128a1/src/client/pydaos/raw/daos_api.py#L523
     */

    // Prepare a DAOS object by generating a DAOS object id.
    daos_obj_id_t oid;
    oid.hi = 0;
    oid.lo = EJFAT_DAOS_OBJECT_ID_LO;
    // std::cout << "oid (init): " << oid.hi <<"," << oid.lo << std::endl; 

    // TODO: study the relationship between object type and OCI.
    // OC_UNKNOWN is encouraged for normal users.
    // Every other OC_XXX (object class identifier, OCI) I tried returns -1003.
    EXPECT_EQ(daos_obj_generate_oid2(coh, &oid, 
        /* object type */ DAOS_OT_KV_LEXICAL,
        /* object class idetifier, check <daos_obj_class.h>. */ OC_UNKNOWN,
        /* hint. */ 0,
        /* reserved arg. uint32_t*/ 0), 0);
    EXPECT_NE(oid.hi, 0);
    EXPECT_EQ(oid.lo, EJFAT_DAOS_OBJECT_ID_LO);
    // std::cout << "oid (assigned): " << oid.hi << "," << oid.lo << std::endl; 
    
    EXPECT_EQ(daos_obj_open(coh, oid, DAOS_OO_RW,
        &oh,
        /* runnning in blockingg mode */ NULL), 0);

    EXPECT_EQ(daos_obj_punch(oh,
        /* optional. DAOS_TX_NONE indicates independent transaction */ DAOS_TX_NONE,
        0, NULL), 0);
    EXPECT_EQ(daos_obj_close(oh, NULL), 0);

    EXPECT_EQ(daos_cont_close(coh, NULL), 0);
    EXPECT_EQ(daos_pool_disconnect(poh, NULL), 0);
    daos_fini();
}
