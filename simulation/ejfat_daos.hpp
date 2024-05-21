/**
 * A simple DAOS client class to send data to daos server via `libdaos` key-value and 
 * `libdfs` dfs_sys interfaces.
 * 
 * First checked in by xmei@jlab.org on May/21/2024.
*/

#include <iostream>
#include <sstream>
#include <iomanip>
#include <fcntl.h> // for O_XXX values
#include <daos.h>
#include <daos_fs_sys.h>

#define MAX_UINT64_DIGITS 20  // help to convert a uint64_t to a padded string

#define EJFAT_DAOS_MAGIC_ERROR_NUM_KV_SETOBJID -6004
#define EJFAT_DAOS_MAGIC_ERROR_NUM_CONNECTOR_EXIT -6002
#define EJFAT_DAOS_MAGIC_ERROR_NUM_DFS_EXIT -6003

#define EJFAT_DFS_DIR_PERMISSION S_IWUSR | S_IRUSR | S_IRGRP | S_IWGRP
#define EJFAT_DFS_FILE_PERMISSION S_IFREG | EJFAT_DFS_DIR_PERMISSION

namespace ejfat {
/** WARNING:
 * This pilot file only has simple use cases!
 * It does not consider the edge cases very well!
*/

    class DAOSConnector
    {
    protected:
        daos_handle_t _poh = DAOS_HDL_INVAL;  // DAOS handle for the Pool.
        daos_handle_t _coh = DAOS_HDL_INVAL;  // DAOS handle for the container.

    private:
        const char* pool_label;
        const char* cont_label;

        void initDAOSConnection(); 
        void closeDAOSConnection();

    public:
        DAOSConnector(const char* pool_label, const char* cont_label);

        ~DAOSConnector();

        /**
         * I can only find the CLI measurement via `dmg pool ls` as below
         * // $ dmg pool ls
         * //     Pool  Size   State Used Imbalance Disabled 
         * //     ----  ----   ----- ---- --------- -------- 
         * //     ejfat 1.7 TB Ready 2%   0%        0/96
        */
        /**
         * Get the pool usage.
         * To be more accurate, it's the tier-0 (SCM) space usage.
         * \return  A float value in range [0, 1] indicating the pool usage.
         */
        /// NOTE: As this usage is reported by pool. Individual receivers needs to have its own pool.
        float getPoolUsage();

        /**
         * Print the connected pool and container label names as "[<pool> <cont>]".
        */
        void printConnectorName() const;

        /**
         * Helper function to process the DAOS error.
         * Print the error number and close the DAOS connection.
         */
        void processError(const char* function_name, const int err_num);
    };

 
    class KVClient : public DAOSConnector
    {
    /**
     * Now every reassembled event is saved as a DAOS KV object, where
     * the lower 64 bits of the object id equal to the ejfat "tick".
     * In each KV object, there is only one KV pair, and the key is based on the
     * event number.
    */
    private:
        daos_obj_id_t _oid = DAOS_OBJ_NIL;

        /**
         * Release the used KV object and handle.
        */
        void resetKVObject();

        /**
         * Set the new DAOS object id based on @param oid_hint.
        */
        void setObjectID(const uint64_t oid_hint);

        /**
         * Helper function to process the KV object error.
         * Print the error number and close the KV object.
         * It only applies to the cases where the object has been successfully opened.
         */
        void processKVError(daos_handle_t oh, const char* op_name, const int err_num);

        /**
         * Close the KV object inddicated by @param _oh.
         * \return The return code. 0 for SUCCESS.
        */
        int close(daos_handle_t oh);

    public:
        ~KVClient() = default;
        KVClient(const char* pool_label, const char* cont_label)
            : DAOSConnector(pool_label, cont_label) { }

        /**
         * Create a DAOS KV object by setting a new object id.
         * \param[in] oid_low   The preset lower 64-bit of the object id.
        */
        void create(uint64_t oid_low);

        /**
         * Push a key-value pair into the KV object.
         * \param[in] key   Associated key. A preset string.
         * \param[in] size  Number of bytes to write.
         * \param[in] buf   Pointer to the user buf.
        */
        void push(const char *key, daos_size_t size, const void *buf) ;

        /**
         * Helper function to print out the full 128-bit DAOS Object ID with a format of <oid.hi>.<oid.lo>.
         * \param[in]   oid The DAOS object id.
        */
        void printObjectID(const daos_obj_id_t oid) const;
    };

    class DFSSysClient : public DAOSConnector
    {
    /**
     * Now we write every reassembled events as an `evt*.dat` file at the root mounting space.
    */
    private:
        dfs_sys_t *_mnt_fs_sys = nullptr;

        int close(dfs_obj_t* dfs_obj);
        void processDFSSysError(dfs_obj_t*, const char *, const int);
     
    public:
        DFSSysClient(const char* pool_label, const char* cont_label);
        ~DFSSysClient() = default;

        /**
         * Create an empty file at the root of the mounting space.
         * \param[in] file_name The name of the file, or the file path.
        */
        void create(const char *file_name);

        /**
         * Write context to a file.
         * This include 3 dfs_sys operations: open->write->close.
         * \param[in] file_name The name of the file, or the file path.
         * \param[in] size      Number of bytes to write.
         * \param[in] buf       Pointer to the context buffer.
        */
        void push(const char *file_name, daos_size_t size, const void *buf); 
    };


    inline int DFSSysClient::close(dfs_obj_t* _obj)
    {
        return dfs_sys_close(_obj);
    }

    inline void DFSSysClient::processDFSSysError(
        dfs_obj_t* _obj, const char* op_name, const int err_num)
    {
        std::cerr << "Error in DAOS DFSSys object operation " << op_name << ": " << err_num << std::endl;
        
        int rt = DFSSysClient::close(_obj);
        if (rt != 0) {
            DFSSysClient::processError("dfs_obj_close", rt);
        }
    }

    DFSSysClient::DFSSysClient(const char *pool_label, const char *cont_label)
        : DAOSConnector(pool_label, cont_label)
    {
        // Mount the container to a POSIX userspace better preset with `dfuse`.
        // This will initialize @param _mnt_fs_sys.
        int rt = dfs_sys_mount(
            _poh, _coh, O_RDWR, DFS_SYS_NO_CACHE, &_mnt_fs_sys);
        if (rt != 0) {
            std::cerr << "Failed to mount the DFS userspace!" << std::endl;
        }
        exit(EJFAT_DAOS_MAGIC_ERROR_NUM_DFS_EXIT);
    }

    inline void DFSSysClient::create(const char *file_name)
    {
        int rt = dfs_sys_mknod(_mnt_fs_sys, file_name,
        /* User and owner can RW */ EJFAT_DFS_FILE_PERMISSION, 0, 0);

        if (rt != 0) {
            DFSSysClient::processError("dfs_sys_mknod", rt);
        }
    }

    inline void DFSSysClient::push(const char *file_name, daos_size_t size, const void *buf)
    {   
        // Open the file object, which will initialize \var _dfs_obj.
        dfs_obj_t *_dfs_obj = nullptr;
        int rt = dfs_sys_open(
            _mnt_fs_sys, file_name, EJFAT_DFS_FILE_PERMISSION, O_RDWR,0, 0, NULL, &_dfs_obj);

        if (rt != 0) {
            DFSSysClient::processError("dfs_sys_open_fileobj", rt);
        }

        // Write to the file object.
        daos_size_t wrt_size = size;  // both in and out for the below function.
        rt = dfs_sys_write(_mnt_fs_sys, _dfs_obj, buf, 0, &wrt_size, NULL);
        if (rt != 0) {
            DFSSysClient::processDFSSysError(_dfs_obj, "dfs_sys_write", rt);
        }

        // Close the file objects.
        rt = DFSSysClient::close(_dfs_obj);
    }

    inline void KVClient::printObjectID(const daos_obj_id_t oid) const
    {
        std::cout << oid.hi << "." << oid.lo << std::endl;
    }

    int KVClient::close(daos_handle_t oh)
    {
        if (oh.cookie == 0) {      // No such object is accepttable.
            KVClient::resetKVObject();
            return 0;
        }

        int rt = daos_kv_close(oh, NULL);
        KVClient::resetKVObject();
        return rt;
    }

    inline void KVClient::resetKVObject()
    {
        // Reset object id and handle for next use.
        _oid = DAOS_OBJ_NIL;
    }

    inline void KVClient::setObjectID(const uint64_t oid_lowbits)
    {
        // Make sure it's a new object that \var _oid.lo is 0.
        if (KVClient::_oid.lo != 0) {
            std::cout << "Not a new object - " << std::endl;
            KVClient::printObjectID(_oid);
            KVClient::processError("daos_kv_obj_set_id", EJFAT_DAOS_MAGIC_ERROR_NUM_KV_SETOBJID);
        }

        KVClient::_oid.lo = oid_lowbits;
    }

    inline void KVClient::processKVError(daos_handle_t oh, const char *op_name, const int err_num)
    {
        std::cerr << "Error in DAOS KV object operation " << op_name << ": " << err_num << std::endl;

        int rt = KVClient::close(oh);
        if (rt != 0) {
            KVClient::processError("daos_kv_obj_close", rt);
        }
    }

    void KVClient::create(const uint64_t oid_low)
    {
        // oid.hi + oid.low is 128-bit. The higher 32-bit of oid.low is reserved for DAOS.
        KVClient::setObjectID(oid_low);
        int rt = daos_obj_generate_oid2(KVClient::_coh, &_oid, 
            /* Object type */ DAOS_OT_KV_LEXICAL,
            /* Object class identifier, check <daos_obj_class.h>. */ OC_UNKNOWN,
            /* Hints. */ 0,
            /* Reserved arg. uint32_t*/ 0);

        if (rt != 0) {
            KVClient::processError("daos_kv_create_obj", rt);
        }

        std::cout << "\nCreate DAOS object: \n  ";
        KVClient::printObjectID(_oid);
    }

    void KVClient::push(const char *key, daos_size_t size, const void *buf)
    {
        daos_handle_t _oh = DAOS_HDL_INVAL;
        // Init object handle.
        /// NOTE: In a KV object there can be multiple KV pairs.
        int rt = daos_kv_open(KVClient::_coh, _oid, DAOS_OO_RW, &_oh, NULL);
        if (rt != 0) {
            KVClient::processError("daos_kv_obj_open", rt);
        }

        rt = daos_kv_put(_oh, DAOS_TX_NONE, 0, key, size, buf,
            /* NULL for running in the blocking mode */ NULL);
        if (rt != 0) {
            KVClient::processKVError(_oh, "daos_kv_obj_put", rt);
        }
        // std::cout << "Put " << size << " bytes of data to "
        //     << _oid.hi << ":" << _oid.lo << " with key_str: " << key << std::endl;

        rt = KVClient::close(_oh);
        if (rt != 0) {
            KVClient::processKVError(_oh, "daos_kv_obj_close", rt);
        }
    }

    /// @brief Helper function to convert a uint64_t to fixed-width string.
    /// @return the converted string.
    const char* uint64ToStringWithPadding(uint64_t num, int width)
    {
        static std::string str; // static to ensure lifetime beyond function scope
        std::stringstream ss;
        ss << std::setw(width) << std::setfill('0') << num;
        str = ss.str();

        std::cout << "\nGet event id string: " << str << std::endl;
        return str.c_str();
    }

    /// @brief Function to generate DAOS object key strings.
    /// @return a string based on @param evt_id but with padding zeros
    const char* generate_daos_kv_key(uint64_t evt_id)
    {
        const char * result = uint64ToStringWithPadding(evt_id, MAX_UINT64_DIGITS);
        return result;
    }

    DAOSConnector::DAOSConnector(const char* pool_label, const char* cont_label) : \
        pool_label(pool_label), cont_label(cont_label)
    {
        DAOSConnector::initDAOSConnection();
        std::cout << "\nOpened DAOS contaner ";
        DAOSConnector::printConnectorName();
    }

    DAOSConnector::~DAOSConnector()
    {
        // Cleanup code here if needed
        DAOSConnector::closeDAOSConnection();
    }

    inline void DAOSConnector::printConnectorName() const
    {
        std::cout << "[" << this->pool_label << " " << this->cont_label <<"]\n\n";
    }

    void DAOSConnector::initDAOSConnection() {
        // If the return value is not 0 (SUCCESS), it's a negative 4-digit error number, i.e., -1xxx.
        // Now we have to serach the original DAOS codebase for the error meaning.
        int rt = -1;

        daos_init();  // must-have

        // Connect to the pool. It will initialze \var poh.
        rt = daos_pool_connect(pool_label, NULL, DAOS_PC_RW, &_poh, NULL, NULL);
        if (rt != 0) {
            DAOSConnector::processError("daos_pool_connect", rt);
        }

        // Connect to the container. It will initialize \var _coh.
        rt = daos_cont_open(_poh, cont_label, DAOS_COO_RW, &_coh, NULL, NULL);
        if (rt != 0) {
            DAOSConnector::processError("daos_cont_open", rt);
        }
    }

    void DAOSConnector::closeDAOSConnection()
    {
        if (_coh.cookie != 0) {
            daos_cont_close(_coh, NULL);
        }

        if (_poh.cookie != 0) {
            daos_pool_disconnect(_poh, NULL);
        }

        daos_fini();  // must-have
        std::cout << "Close DAOS container... ";
        DAOSConnector::printConnectorName();

        // Cleanup.
        _coh = DAOS_HDL_INVAL;
        _poh = DAOS_HDL_INVAL;
    }

    // Process the error code returned by daos.h and exit.
    void DAOSConnector::processError(const char* function_name, const int err_num)
    {
        /// TODO: elegant error handling instead of violate exit.
        std::cerr << "Error in DAOS function " << function_name << ": " << err_num << std::endl;
        
        DAOSConnector::closeDAOSConnection();
        exit(EJFAT_DAOS_MAGIC_ERROR_NUM_CONNECTOR_EXIT); 
    }

    float DAOSConnector::getPoolUsage()
    {
        daos_pool_info_t pool_info = {0};
        pool_info.pi_bits = DPI_SPACE;  // must-have. Tell daos_pool_query() to return space info.

        int rt = daos_pool_query(_poh, NULL, &pool_info, NULL, NULL);
        if (rt != 0) {
            DAOSConnector::processError("daos_pool_query", rt);
        }

        float tier0_usage = 1.F - \
            (float)pool_info.pi_space.ps_space.s_free[0] / pool_info.pi_space.ps_space.s_total[0];

        return tier0_usage;
    }
}
