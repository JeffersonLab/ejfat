//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * A simple DAOS client class to send data to daos server via key-value object stores.
*/

#include <daos.h>
#include <iostream>
#include <sstream>
#include <iomanip>

#define MAX_UINT64_DIGITS 20  // help to convert a uint64_t to a padded string

namespace ejfat {

    class DAOSConnector {

        /** WARNING:
         * This pilot class only has simple use cases!
         * It does not consider the edge cases very well!
        */
        
        private:
            const char* pool_label;
            const char* cont_label;

            daos_handle_t poh, coh = DAOS_HDL_INVAL;  // DAOS handles, init with value {0}

            void closeDAOSConnection() {
                if (coh.cookie != 0) {
                    daos_cont_close(coh, NULL);
                }

                if (poh.cookie != 0) {
                    daos_pool_disconnect(poh, NULL);
                }

                daos_fini();  // must-have
                std::cout << "Close DAOS container [" << pool_label << ":" << cont_label << "]\n\n";

                // Cleanup
                coh = DAOS_HDL_INVAL;
                poh = DAOS_HDL_INVAL;
                
                free((void*)pool_label);
                free((void*)cont_label);
            }

            int closeDAOSKVObject(daos_handle_t oh) {
                // No such objects
                if (oh.cookie == 0) {
                    return 0;
                }

                return daos_kv_close(oh, NULL);
            }

            // Print the error number and close the DAOS connection
            void processRtError(const char* function_name, const int err_num) {
                /// TODO: make a lookup table for the return code.
                std::cerr << "Error in DAOS function " << function_name << ": " << err_num << std::endl;
                
                closeDAOSConnection();

                /// TODO: elegant error handling instead of violate exit.
                exit(-6002); 
            }

            // Print the error number and close the KV object.
            // It only applies to the cases where the object has been successfully opened.
            void processKVError(daos_handle_t oh, const char* op_name, const int err_num) {
                /// TODO: make a lookup table for the return code.
                std::cerr << "Error in DAOS KV object operation " << op_name << ": " << err_num << std::endl;
               
                int rt = closeDAOSKVObject(oh);
                if (rt != 0) {
                    processRtError("daos_kv_obj_close", rt);
                }
            }

            // Init the DAOS connection
            void initDAOSConnection() {
                // If the return value is not 0 (SUCCESS), it's a negative 4-digit error number, i.e., -1xxx.
                // Now we have to serach the original DAOS codebase for the error meaning.
                int rt = -1;

                daos_init();  // must-have

                // Connect to the pool. It will initialze @param poh.
                rt = daos_pool_connect(pool_label, NULL, DAOS_PC_RW, &poh, NULL, NULL);
                if (rt != 0) {
                    processRtError("daos_pool_connect", rt);
                }

                // Connect to the container. It will initialize @param coh.
                rt = daos_cont_open(poh, cont_label, DAOS_COO_RW, &coh, NULL, NULL);
                if (rt != 0) {
                    processRtError("daos_cont_open", rt);
                }
            }

            // Set the new DAOS object id based on @parm tick. Make oid.lo = tick.
            void setKVObjectID(const uint64_t tick, daos_obj_id_t *oid) {
                if (oid->lo != 0) {
                    processKVError(DAOS_HDL_INVAL, "daos_obj_set_id", -6004);  // magic error number
                }

                oid->lo = tick;
            }

        public:
            // Constructor
            DAOSConnector(const char* pool_label, const char* cont_label) : \
                pool_label(pool_label), cont_label(cont_label) {
                
                initDAOSConnection();
                std::cout << "\nOpened DAOS contaner [" << pool_label << " " << cont_label << "]\n" << std::endl;
            }

            // Destructor
            ~DAOSConnector() {
                // Cleanup code here if needed
                closeDAOSConnection();
            }

            // Create a new key-value DAOS object, return the object id.
            daos_obj_id_t createKVObject(const uint64_t ejfat_tick) {
                daos_obj_id_t oid = DAOS_OBJ_NIL;   // a unique object id.

                // oid.hi and oid.low are 128-bit. The high 32-bit of oid.low is reserved for DAOS.
                setKVObjectID(ejfat_tick, &oid);
                int rt = daos_obj_generate_oid2(coh, &oid, 
                    /* object type */ DAOS_OT_KV_LEXICAL,
                    /* object class idetifier, check <daos_obj_class.h>. */ OC_UNKNOWN,
                    /* hint. */ 0,
                    /* reserved arg. uint32_t*/ 0);

                if (rt != 0) {
                    processRtError("daos_obj_create", rt);
                }

                std::cout << "\nCreate DAOS object (oid.hi:oid.lo) - " << oid.hi << ":" << oid.lo << std::endl;
                return oid;
            }

            // Push the data in @param buf into KV object.
            // @param key, @size must be preset.
            void push2KVObject(daos_obj_id_t oid,
                        const char *key, daos_size_t size, const void *buf
                        ) {
                daos_handle_t oh = DAOS_HDL_INVAL;
                int rt = -1;
                
                // Init @val oh.
                rt = daos_kv_open(coh, oid, DAOS_OO_RW, &oh, NULL);
                if (rt != 0) {
                    processRtError("daos_kv_obj_open", rt);
                }

                rt = daos_kv_put(oh, DAOS_TX_NONE, 0, key, size, buf,
                    /* running in blocking mode */ NULL);
                if (rt != 0) {
                    processKVError(oh, "daos_kv_obj_put", rt);
                }
                // std::cout << "Put " << size << " bytes of data to "
                //     << oid.hi << ":" << oid.lo << " with key_str: " << key << std::endl;

                rt = closeDAOSKVObject(oh);
                if (rt != 0) {
                    processKVError(oh, "daos_kv_obj_close", rt);
                }
            }

            
            /**
             * Get the pool percentage usage. Now it's based on the tier-0 (SCM) space info.
             * @return a float value in range [0, 1]
             */
            /* Now I can only find a measurement "Used" via `dmg pool ls` as below
                //  $ dmg pool ls
                //     Pool  Size   State Used Imbalance Disabled 
                //     ----  ----   ----- ---- --------- -------- 
                //     ejfat 1.7 TB Ready 2%   0%        0/96
            */
            /// NOTE: As this usage is reported by pool. Individual receivers needs to have its own pool.
            float getPoolUsage() {
                daos_pool_info_t pool_info = {0};
                pool_info.pi_bits = DPI_SPACE;  // must-have. Tell daos_pool_query() to return space info.

                int rt = daos_pool_query(poh, NULL, &pool_info, NULL, NULL);
                if (rt != 0) {
                    processRtError("daos_pool_query", rt);
                }

                float tier0_usage = 1.F - \
                    (float)pool_info.pi_space.ps_space.s_free[0] / pool_info.pi_space.ps_space.s_total[0];

                return tier0_usage;
            }

    };

    /// @brief Helper function to convert a uint64_t to fixed-width string.
    /// @return the converted string.
    const char* uint64ToStringWithPadding(uint64_t num, int width) {
        static std::string str; // static to ensure lifetime beyond function scope
        std::stringstream ss;
        ss << std::setw(width) << std::setfill('0') << num;
        str = ss.str();

        std::cout << "\nGet event id string: " << str << std::endl;
        return str.c_str();
    }

    /// Helper function to generate DAOS object key strings.
    /// @return a string based on @param evt_id but with padding zeros
    const char* generate_daos_kv_key(uint64_t evt_id) {
        const char * result = uint64ToStringWithPadding(evt_id, MAX_UINT64_DIGITS);
        return result;
    }

}
