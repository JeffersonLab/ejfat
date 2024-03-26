/**
 * DAOS key-value store operation.
 * 
 * First checked in by xmei@jlab.org on Mar/28/2024.
*/

#include <gtest/gtest.h>
#include <daos.h>
#include <iostream>  // for some commented std::cout

// Make sure this pool exists. Query with `dmg sys query list-pools`.
#define EJFAT_DAOS_POOL_LABEL "ejfat"

// Make sure this container exists. List the conts in a DAOS pool by `daos cont ls <pool_label>`. 
#define EJFAT_DAOS_CONT_LABEL "cont1"

#define EJFAT_DAOS_OBJECT_ID_LO 1
#define EJFAT_DAOS_KV_KEY "99999999"  // could be something related to eventID or timestamp
#define EJFAT_DAOS_KV_VALUE "Hello, DAOS!"

/**
 * Create a key-value DAOS object. Put some value into it with a transaction.
 * Commit the transaction. Close the object.
 * Reopen the object via the object id. Get the value by the given key.
 * Check whether the returned value is the same as the put one.
*/
TEST(DaosTest, KeyValueGetAfterPut) {
    daos_init();

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    daos_handle_t poh, coh, oh1, th = DAOS_HDL_INVAL;  // init with value {0}

    EXPECT_EQ(daos_pool_connect(pool_label, NULL, DAOS_PC_RW, &poh, NULL, NULL), 0);
    EXPECT_EQ(daos_cont_open(poh, cont_label, DAOS_COO_RW, &coh, NULL, NULL), 0);

    // Prepare a DAOS object by generating a DAOS object id.
    daos_obj_id_t oid;
    oid.hi = 0;
    oid.lo = EJFAT_DAOS_OBJECT_ID_LO;

    // TODO: study the relationship between object type and OCI.
    // Every other OC_XXX (object class identifier, OCI) I tried return -1003
    EXPECT_EQ(daos_obj_generate_oid2(coh, &oid,
        /* object type */ DAOS_OT_KV_LEXICAL,
        /* object class idetifier, check <daos_obj_class.h>. */ OC_UNKNOWN,
        /* hint. */ 0,
        /* reserved arg. uint32_t*/ 0), 0);
    EXPECT_NE(oid.hi, 0);   // oid is updated
    EXPECT_EQ(oid.lo, EJFAT_DAOS_OBJECT_ID_LO);
    // std::cout << "oid (assigned): " << oid.hi << "," << oid.lo << std::endl; 
    
    // Start a new transaction. Returns the transaction handle @var th. 
    EXPECT_EQ(daos_tx_open(coh, &th, 
        /* DAOS transaction flags, DAOS_TF_RONLY or DAOS_TF_ZERO_COPY */ DAOS_TF_ZERO_COPY,
        /* runnning in blockingg mode */ NULL), 0);
    
    EXPECT_EQ(daos_kv_open(coh, oid, DAOS_OO_RW,
        &oh1,
        /* runnning in blockingg mode */ NULL), 0);
    EXPECT_GT(oh1.cookie, 0);

    const char *put_key = EJFAT_DAOS_KV_KEY;
    const char *put_value = EJFAT_DAOS_KV_VALUE;
    daos_size_t put_size = strlen(EJFAT_DAOS_KV_VALUE);

    ASSERT_EQ(daos_kv_put(oh1, th, 0, put_key, put_size,
        /* type cast to void*. Optional. */ (void *)put_value,
        /* running in blocking mode */ NULL), 0);

    // Commit the trasaction, close the object & transaction.
    ASSERT_EQ(daos_tx_commit(th, NULL), 0);
    EXPECT_EQ(daos_kv_close(oh1, NULL), 0);
    EXPECT_EQ(daos_tx_close(th, NULL), 0);

    /**
     * Open the key-value object again.
     * Need the oid of the created DAOS object.
     * And it creates another object handle.
    */
    daos_handle_t oh2 = DAOS_HDL_INVAL;
    // std::cout << "oh1: \t" << oh1.cookie << std::endl;
    // std::cout << "oid: \t" << oid.hi << ", " << oid.lo << std::endl; 
    EXPECT_EQ(daos_kv_open(coh, oid, DAOS_OO_RO, &oh2, NULL), 0);
    // std::cout << "oh2: \t" << oh2.cookie << std::endl;
    EXPECT_NE(oh1.cookie, oh2.cookie);

    daos_size_t fetch_size = DAOS_REC_ANY;
    EXPECT_EQ(daos_kv_get(oh2, DAOS_TX_NONE, 0, put_key, &fetch_size, 
        /* NULL means only returns the fetch_size */ NULL, NULL), 0);
    EXPECT_EQ(fetch_size, put_size);

    char fetch_buf[fetch_size];
    EXPECT_EQ(daos_kv_get(oh2, DAOS_TX_NONE, 0, put_key, &fetch_size,
        fetch_buf, NULL), 0);
    // std::cout << "fetch_buf: " << fetch_buf << std::endl;
    EXPECT_STREQ(fetch_buf, put_value);  // verify we get the same content as the put value.

    EXPECT_EQ(daos_kv_close(oh2, NULL), 0);
    EXPECT_EQ(daos_cont_close(coh, NULL), 0);
    EXPECT_EQ(daos_pool_disconnect(poh, NULL), 0);

    daos_fini();
}

TEST(DaosTest, KeyValueMultiplePuts) {
     daos_init();

    const char *pool_label = EJFAT_DAOS_POOL_LABEL;
    const char *cont_label = EJFAT_DAOS_CONT_LABEL;

    daos_handle_t poh, coh, oh = DAOS_HDL_INVAL;  // init with value {0}

    EXPECT_EQ(daos_pool_connect(pool_label, NULL, DAOS_PC_RW, &poh, NULL, NULL), 0);
    EXPECT_EQ(daos_cont_open(poh, cont_label, DAOS_COO_RW, &coh, NULL, NULL), 0);

    // Prepare a DAOS object by generating a DAOS object id.
    daos_obj_id_t oid;
    oid.hi = 0;
    oid.lo = EJFAT_DAOS_OBJECT_ID_LO;

    // TODO: study the relationship between object type and OCI.
    // Every other OC_XXX (object class identifier, OCI) I tried return -1003
    EXPECT_EQ(daos_obj_generate_oid2(coh, &oid, 
        /* object type */ DAOS_OT_KV_LEXICAL,
        /* object class idetifier, check <daos_obj_class.h>. */ OC_UNKNOWN,
        /* hint. */ 0,
        /* reserved arg. uint32_t*/ 0), 0);
    EXPECT_NE(oid.hi, 0);

    EXPECT_EQ(daos_kv_open(coh, oid, DAOS_OO_RW, &oh, NULL), 0);

    int n = 5;
    std::string base_key_str = "test-key-";
    std::string base_value_str = "This is string ";
    for (int i = 0; i < n; i++) {
        std::string put_key = base_key_str + std::to_string(i);
        std::string put_value = base_value_str + std::to_string(i);
        
        ASSERT_EQ(daos_kv_put(oh, DAOS_TX_NONE, 0, put_key.c_str(), put_value.size(),
        /* type cast to void*. Optional. */  static_cast<const void*>(put_value.data()),
        /* running in blocking mode */ NULL), 0);
    }
    
    for (int i = 0; i < n; i++) {
        std::string get_key = base_key_str + std::to_string(i);
        daos_size_t fetch_size = DAOS_REC_ANY;
        
        ASSERT_EQ(daos_kv_get(oh, DAOS_TX_NONE, 0, get_key.c_str(), &fetch_size, 
        /* NULL means only returns the fetch_size */ NULL, NULL), 0);
        ASSERT_EQ(fetch_size, 16);

        char buf[fetch_size];
        ASSERT_EQ(daos_kv_get(oh, DAOS_TX_NONE, 0, get_key.c_str(), &fetch_size, buf, NULL), 0);
        // std::cout << "get_str: " << buf << std::endl;
    }

    EXPECT_EQ(daos_kv_close(oh, NULL), 0);
    EXPECT_EQ(daos_cont_close(coh, NULL), 0);
    EXPECT_EQ(daos_pool_disconnect(poh, NULL), 0);

    daos_fini();
}
