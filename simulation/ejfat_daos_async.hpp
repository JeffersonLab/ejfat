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

#include "ejfat_daos.hpp"

// BoE calulation: DAOS dfs chunk size (1MiB)/ Ethernet MTU - header (8952B) = 117.13
/// TODO: 256 used to be the best on Aug-2 but now the maximum supported is 256!!!
/// TODO: Dig it later (should be related to the RDMA library CART)

// For DFS, maximum can set to 32;
// For KV, this can be up to 300. 500 will fail. Larger than 256 does not gain more perf.
#define EJFAT_DAOS_EVT_QUEUE_SIZE 256

#define EJFAT_DAOS_MAGIC_ERROR_NUM_EQ_EXIT -6007

#define MiB 1048576

namespace ejfat {
/** WARNING:
 * This pilot file only has simple use cases!
 * It does not consider the edge cases very well!
*/

class DAOSConnectorAsync
{
protected:
    daos_handle_t _poh = DAOS_HDL_INVAL;  // DAOS handle for the Pool
    daos_handle_t _coh = DAOS_HDL_INVAL;  // DAOS handle for the container

    daos_handle_t _eq; // DAOS event queue
    struct daos_event _evs[EJFAT_DAOS_EVT_QUEUE_SIZE]; // Array of DAOS event

    daos_size_t cap_size; // the upper limit of accumulated bytes

private:
    const char* pool_label;
    const char* cont_label;

    void initDAOSConnection();
    void closeDAOSConnection();

    void initDAOSEvents();
    void destroyDAOSEvents();

public:
    uint64_t currEventId = 0;
    daos_size_t acummBytes = 0;  // total bytes have writen belong to this DAOS event queue

    void setCapSize(daos_size_t size) {cap_size = size;};

    DAOSConnectorAsync(
        const char* pool_label, const char* cont_label); // default to 1 MiB

    ~DAOSConnectorAsync();

    /**
     * Get the pool usage.
     * To be more accurate, it's the tier-0 (SCM) space usage.
     * \return  A float value in range [0, 1] indicating the pool usage.
     */
    float getPoolUsage();

    /**
     * Check whether the tasks in the DAOS event queue are all completed.
     * \param[in]   num The number of events assumed have completed.
     * \return      Whether all the events have completed.s
     */
    bool syncDAOSEventQueue(int num);

    void resetDAOSEventQueueCounters();

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

class DFSSysClientAsync : public DAOSConnectorAsync
{
/**
 * Write the receiving UDP packets into ~1MiB (the nearest number to 1MiB) fs_sys files.
 * Multiple UDP packets write to one fs_sys object until the chunk size is reached.
*/
private:
    uint64_t fileId = 1;  // related to filename, so start with 1.
    /// TODO: Now it's set to default value 1MiB. Tune it to see the effect!!!
    daos_size_t _chunk_size = MiB;

    dfs_sys_t* _mnt_fs_sys = nullptr;
    dfs_obj_t* currFileObj;  // points to a DFSSys file object

    void processDFSSysError(const char *, const int);

    /**
     * Set the file path string.
     * \param[in]   counter The filename indicator.
     * \return      The full file path loacted at the root mounting dir.
     *              It will look like "<mnt_dir>/<counter>.dat"
    */
    std::string setFullFilePath(const uint64_t counter);

    /**
     * Create an empty file at the root of the mounting space.
     * This will initilize \var currFileObj
     * \param[in] counter A filename indicator.
    */
    void createFile(const uint64_t counter);

    /**
     * Commit a file object (stop writing to it)
     * This involves a DAOS event queue poll, and then close the file object.
     * \param[in] num_events Assumed completed events.
    */
    void commitFile(int num_events);

    /**
     * Write context to \var currFileObj withs offsets.
     * \param[in] size      Number of bytes to write.
     * \param[in] buf       Pointer to the context buffer.
     * \param[in] offset    Offset to write the file.
    */
    void writeFile(daos_size_t size, const void *buf, daos_off_t offset);

public:
    DFSSysClientAsync(
        const char* pool_label, const char* cont_label);
    ~DFSSysClientAsync() = default;

    /**
     * Flush memory buf into DAOS server.
     * \param[in]   counter EJFAT packets counter.
     * \param[in]   size    The length of memory buffer.
     * \param[in]   buf     Piece of memory to write.
     */
    void flush(uint64_t counter, daos_size_t size, const void *buf);
};


class KVClientAsync : public DAOSConnectorAsync
{
/**

*/
private:
    int objectId = 0;  // the objectId-th object to create.
    daos_handle_t currObjectHandle = DAOS_HDL_INVAL;
    daos_obj_id_t currObject;

    void setObjectID(const uint64_t oid_lowbits);

    /**
     * Stop writing to the current object and sync the DAOS event queue to make sure
     * all previous events have completed.
     * Close this object afterwardds.
     *
     * \param[in]   num_events      The number of uncompleted events in the DAOS event queue.
     */
    void commitObject(int num_events);

    /**
     * Set the new DAOS object id based on @param oid_hint.
     * Open this object.
    */
    void createObject(const uint64_t oid_hint);

    /**
     * Helper function to process the KV object error.
     * Print the error number and close the KV object.
     * It only applies to the cases where the object has been successfully opened.
     */
    void processKVError(const char* op_name, const int err_num);

public:
    ~KVClientAsync() = default;
    KVClientAsync(const char* pool_label, const char* cont_label)
        : DAOSConnectorAsync(pool_label, cont_label) { }

    /**
     * Flush memory buf into DAOS server.
     * \param[in]   counter EJFAT packets counter.
     * \param[in]   size    The length of memory buffer.
     * \param[in]   buf     Piece of memory to write.
     */
    void flush(uint64_t counter, daos_size_t size, const void *buf);

    /**
     * Helper function to print out the full 128-bit DAOS Object ID with a format of <oid.hi>.<oid.lo>.
     * \param[in]   oid The DAOS object id.
    */
    void printObjectID(const daos_obj_id_t oid) const;
    };


DAOSConnectorAsync::DAOSConnectorAsync(
    const char* pool_label, const char* cont_label) :
    pool_label(pool_label), cont_label(cont_label)
{
    initDAOSConnection();
    std::cout << "\nOpened DAOS container ";
    printConnectorName();

    initDAOSEvents();
}

void DAOSConnectorAsync::initDAOSEvents()
{
    int rc = daos_eq_create(&_eq);  // get DAOS event queue handle
    if (rc != 0) {
        DAOSConnectorAsync::processError("daos_eq_create", rc);
    }

    // Initialize events
    for (int i = 0; i < EJFAT_DAOS_EVT_QUEUE_SIZE; i++) {
        int rc = daos_event_init(&_evs[i], _eq, NULL);
        if (rc != 0) {
            std::cout << "Error init event: " << i << std::endl;
            DAOSConnectorAsync::processError("daos_eq_create ", rc);
        }
    }
    std::cout << "Init DAOS event queue succeeded... eq.cookie: " << _eq.cookie << std::endl;
    std::cout << "  Queue size: " << EJFAT_DAOS_EVT_QUEUE_SIZE << std::endl;
}

bool DAOSConnectorAsync::syncDAOSEventQueue(int num)
{
    // std::cout << "\nBegin polling the events...  eq.cookie: " << _eq.cookie << std::endl;

    int readyEvents = 0;
    for (int i = 0; i < EJFAT_DAOS_EVT_QUEUE_SIZE; i++) {
        bool isReady;
        int rc = daos_event_test(&_evs[i], DAOS_EQ_WAIT, &isReady);
        if (rc != 0) {
            std::cerr << "\tFailed to test event: " << i << ", rc: " << rc << std::endl;
            exit(EJFAT_DAOS_MAGIC_ERROR_NUM_EQ_EXIT);
        }
        if (!isReady) {
            std::cerr << "\tFailed to test event: " << i << ", isReady: " << isReady << std::endl;
            exit(EJFAT_DAOS_MAGIC_ERROR_NUM_EQ_EXIT);
        }
        readyEvents += isReady;
    }

    // Reset the events
    for (int i = 0; i < EJFAT_DAOS_EVT_QUEUE_SIZE; i++){
        // std::cout << "\t\t Finalizing  event: " << i << std::endl;
        int rc = daos_event_fini(&_evs[i]);
        if (rc != 0) {
            std::cout << "Failed to finalize event: " << i << ", rc:" << rc << std::endl;
            exit(EJFAT_DAOS_MAGIC_ERROR_NUM_EQ_EXIT);
        }
        rc = daos_event_init(&_evs[i], _eq, NULL);
        if (rc != 0) {
            std::cout << "Failed to re-init event: " << i << ", rc:" << rc << std::endl;
            exit(EJFAT_DAOS_MAGIC_ERROR_NUM_EQ_EXIT);
        }
    }
    // std::cout << "\t Re-initialized events\n\n";

    return readyEvents == num;
}

void DAOSConnectorAsync::destroyDAOSEvents() {
    // Release all events
	for (int i = 0; i < EJFAT_DAOS_EVT_QUEUE_SIZE; i++) {
	    daos_event_fini(&_evs[i]);
	}
    // destroy event queue
    daos_eq_destroy(_eq, 0);
}

void DAOSConnectorAsync::resetDAOSEventQueueCounters()
{
    // std::cout << "\t Wrote packets: " << currEventId << ", KiB: " << float(acummBytes) / 1024.0 << std::endl;

    currEventId = 0;
    acummBytes = 0;
}

DAOSConnectorAsync::~DAOSConnectorAsync()
{
    // Cleanup code here if needed
    DAOSConnectorAsync::closeDAOSConnection();
}

inline void DAOSConnectorAsync::printConnectorName() const
{
    std::cout << "[" << this->pool_label << " " << this->cont_label <<"]\n\n";
}

void DAOSConnectorAsync::initDAOSConnection() {
    // If the return value is not 0 (SUCCESS), it's a negative 4-digit error number, i.e., -1xxx.
    // Now we have to serach the original DAOS codebase for the error meaning.
    int rc= -1;

    daos_init();  // must-have

    // Connect to the pool. It will initialze \var poh.
    rc = daos_pool_connect(pool_label, NULL, DAOS_PC_RW, &_poh, NULL, NULL);
    if (rc != 0) {
        DAOSConnectorAsync::processError("daos_pool_connect", rc);
    }

    // Connect to the container. It will initialize \var _coh.
    rc = daos_cont_open(_poh, cont_label, DAOS_COO_RW, &_coh, NULL, NULL);
    if (rc != 0) {
        DAOSConnectorAsync:processError("daos_cont_open", rc);
    }
}

void DAOSConnectorAsync::closeDAOSConnection()
{
    DAOSConnectorAsync::destroyDAOSEvents();

    // Close all the handles to DAOS cont and pool.
    if (_coh.cookie != 0) {
        daos_cont_close(_coh, NULL);
    }

    if (_poh.cookie != 0) {
        daos_pool_disconnect(_poh, NULL);
    }

    daos_fini();  // must-have
    std::cout << "Close DAOS container... ";
    DAOSConnectorAsync::printConnectorName();

    // Cleanup.
    _coh = DAOS_HDL_INVAL;
    _poh = DAOS_HDL_INVAL;
}

// Process the error code returned by daos.h and exit.
void DAOSConnectorAsync::processError(const char* function_name, const int err_num)
{
    /// TODO: elegant error handling instead of violate exit.
    std::cerr << "Error in DAOS function " << function_name << ": " << err_num << std::endl;

    DAOSConnectorAsync::closeDAOSConnection();
    exit(EJFAT_DAOS_MAGIC_ERROR_NUM_CONNECTOR_EXIT);
}

float DAOSConnectorAsync::getPoolUsage()
{
    daos_pool_info_t pool_info = {0};
    pool_info.pi_bits = DPI_SPACE;  // must-have. Tell daos_pool_query() to return space info

    int rc = daos_pool_query(_poh, NULL, &pool_info, NULL, NULL);
    if (rc != 0) {
        DAOSConnectorAsync::processError("daos_pool_query", rc);
    }

    float tier0_usage = 1.F - \
        (float)pool_info.pi_space.ps_space.s_free[0] / pool_info.pi_space.ps_space.s_total[0];

    return tier0_usage;
}

DFSSysClientAsync::DFSSysClientAsync(
    const char *pool_label, const char *cont_label)
    : DAOSConnectorAsync(pool_label, cont_label)
{
    // Mount the container to a POSIX userspace better preset with `dfuse`.
    // This will initialize @param _mnt_fs_sys.
    int rc = dfs_sys_mount(
        _poh, _coh, O_RDWR, DFS_SYS_NO_CACHE, &_mnt_fs_sys);
    if (rc != 0) {
        std::cerr << "Failed to mount the DFS userspace!" << std::endl;
        exit(EJFAT_DAOS_MAGIC_ERROR_NUM_DFS_EXIT);
    }
}

std::string DFSSysClientAsync::setFullFilePath(const uint64_t counter)
{
    const char* prefix = "/";  // create at the root dir.
    const char* suffix = ".dat";

    std::string filePath = prefix + std::to_string(counter) + suffix;

    return filePath;
}

void DFSSysClientAsync::createFile(uint64_t counter)
{
    std::string filePath = DFSSysClientAsync::setFullFilePath(counter);

    int rc = dfs_sys_mknod(_mnt_fs_sys, filePath.c_str(),
        /* User and owner can RW */ EJFAT_DFS_FILE_PERMISSION, 0,
        /* Chunk size, tunnable!!! 0 for default 1MiB */ 0);
    if (rc != 0) {
        DFSSysClientAsync::processError("dfs_sys_mknod", rc);
    }

    // Open the file object, which will initialize \var currFileObj.
    rc = dfs_sys_open(_mnt_fs_sys, filePath.c_str(),
        EJFAT_DFS_FILE_PERMISSION, O_RDWR, 0, 0, NULL, &currFileObj);
    if (rc != 0) {
        DFSSysClientAsync::processError("dfs_sys_open", rc);
    }

    if (currFileObj == NULL) {
        DFSSysClientAsync::processError("null fileObj", EJFAT_DAOS_MAGIC_ERROR_NUM_DFS_EXIT);
    }

    // if (counter % 1000 == 1) {
    //     std::cout << "\nFile create succeeded:   " << filePath << std::endl;
    // }
}


inline void DFSSysClientAsync::processDFSSysError(const char* op_name, const int err_num)
{
    std::cerr << "Error in DAOS DFSSys object operation " << op_name << ": " << err_num << std::endl;

    int rc = dfs_sys_close(currFileObj);
    if (rc != 0) {
        DFSSysClientAsync::processError("dfs_obj_close", rc);
    }
}

inline void DFSSysClientAsync::writeFile(
    daos_size_t size, const void *buf, daos_off_t offset)
{
    // Write to the file object when it will not exceed the chunk limit.
    daos_size_t wrt_size = size;  // both in and out for the below function.
    // std::cout << "\t Write async..." << currEventId << std::endl;
    int rc = dfs_sys_write(_mnt_fs_sys, currFileObj, buf,
                        /* Offset */ offset, &wrt_size, &_evs[currEventId]);
    // std::cout << "\t [write_async] return: " << wrt_size << std::endl;
    if (rc != 0) {
        DFSSysClientAsync::processDFSSysError("dfs_sys_write_async", rc);
    }
}

void DFSSysClientAsync::commitFile(int num_evts)
{
    // std::cout << "\nBegin commit file, fileId: " << fileId << std::endl;
    bool queueReady = DAOSConnectorAsync::syncDAOSEventQueue(num_evts);
    if (!queueReady) {
        DFSSysClientAsync::processDFSSysError(
            "commitFile", EJFAT_DAOS_MAGIC_ERROR_NUM_EQ_EXIT);
    } else {
        int rc = dfs_sys_close(currFileObj);
        if (rc != 0) {
            DFSSysClientAsync::processDFSSysError("dfs_sys_close", rc);
        }
        currFileObj = nullptr;

    // if (fileId % 1000 == 1)
    //     std::cout << "\t Commit file" << std::endl;
    }
}

void DFSSysClientAsync::flush(uint64_t counter, daos_size_t size, const void *buf)
{
    // The first file to write.
    if (fileId == 1 && DFSSysClientAsync::DAOSConnectorAsync::acummBytes == 0)
    {
        DFSSysClientAsync::createFile(fileId);
    }

    // Planned write can fit into the file.
    // if (DFSSysClientAsync::acummBytes + size <= cap_size)
    if (currEventId < EJFAT_DAOS_EVT_QUEUE_SIZE)
    {
        DFSSysClientAsync::writeFile(
            size, buf, DFSSysClientAsync::acummBytes);
    } else {
        // std::cout << "\t Reach limit at KiB: "<< float(DFSSysClientAsync::acummBytes)/1024.0 <<\
            ", EvtId: " << currEventId << \
            ", fileId: " << fileId << \
            std::endl;
        // Commit the old file and reset all the counnters of the DAOS event queue.
        DFSSysClientAsync::commitFile(DFSSysClientAsync::currEventId);
        DAOSConnectorAsync::resetDAOSEventQueueCounters();

        // Create a new file and write to it.
        fileId += 1;
        DFSSysClientAsync::createFile(fileId);
        DFSSysClientAsync::writeFile(size, buf, 0);
    }

    // Update the counnters of the DAOS event queue.
    DFSSysClientAsync::acummBytes += size;
    DFSSysClientAsync::currEventId += 1;
}

void KVClientAsync::commitObject(int num_evts)
{
    if (currObjectHandle.cookie == 0) {      // No such object is accepttable.
        currObject = DAOS_OBJ_NIL;
        return;
    }

    // Poll the events in the event queue
    // std::cout << "\nBegin commit object, objectId: " << objectId << std::endl;
    bool queueReady = DAOSConnectorAsync::syncDAOSEventQueue(num_evts);
    if (!queueReady) {
        KVClientAsync::processKVError(
            "commitObject", EJFAT_DAOS_MAGIC_ERROR_NUM_EQ_EXIT);
    }

    // Close current object in BLOCKING mode.
    int rc = daos_kv_close(currObjectHandle, NULL);
    if (rc != 0) {
        std::cout << "Closing object...   obj.cookie: " << currObjectHandle.cookie << std::endl;
        KVClientAsync::processKVError("daos_kv_close", rc);
    }
    currObject = DAOS_OBJ_NIL;

    // if (objectId % 1000 == 0) {
    //     std::cout << "\t Commit Object, objId: " << objectId << std::endl;
    // }
}

inline void KVClientAsync::setObjectID(const uint64_t oid_lowbits)
{
    // Make sure it's a new object that \var _oid.lo is 0.
    if (KVClientAsync::currObject.lo != 0) {
        std::cerr << "Not a new object - " << std::endl;
        KVClientAsync::printObjectID(currObject);
        KVClientAsync::processError("daos_kv_obj_set_id", EJFAT_DAOS_MAGIC_ERROR_NUM_KV_SETOBJID);
    }

    KVClientAsync::currObject.lo = oid_lowbits;
}

void KVClientAsync::createObject(const uint64_t oid_low)
{
    // oid.hi + oid.low is 128-bit. The higher 32-bit of oid.low is reserved for DAOS.
    KVClientAsync::setObjectID(oid_low);
    int rc = daos_obj_generate_oid2(KVClientAsync::_coh, &currObject,
        /* Object type */ DAOS_OT_KV_LEXICAL,
        /* Object class identifier, check <daos_obj_class.h>. */ OC_UNKNOWN,
        /* Hints. */ 0,
        /* Reserved arg. uint32_t*/ 0);

    if (rc != 0) {
        KVClientAsync::processError("daos_kv_create_obj", rc);
    }

    // Open the object in BLocking mode. This will initilizee \var currObjectHandle.
    rc = daos_kv_open(KVClientAsync::_coh, currObject, DAOS_OO_RW, &currObjectHandle, NULL);
    if (rc != 0) {
        KVClientAsync::processError("daos_kv_obj_open", rc);
    }

    // std::cout << "\nCreate DAOS object...   obj.cookie: " << currObjectHandle.cookie << "\n";
    // KVClientAsync::printObjectID(currObject);
}

void KVClientAsync::flush(uint64_t counter, daos_size_t size, const void *buf)
{
    // The first file to write.
    if (currEventId == 0 && DFSSysClientAsync::DAOSConnectorAsync::acummBytes == 0)
    {
        KVClientAsync::createObject(counter);
    }

    // Planned write can fit into the file.
    // if (DFSSysClientAsync::acummBytes + size <= cap_size)
    if (currEventId == EJFAT_DAOS_EVT_QUEUE_SIZE)
    {
        // std::cout << "\t Reach limit at KiB: "<< float(DFSSysClientAsync::acummBytes)/1024.0 <<\
            ", EvtId: " << currEventId << \
            ", fileId: " << fileId << \
            std::endl;
        // Commit the old file and reset all the counnters of the DAOS event queue.
        KVClientAsync::commitObject(KVClientAsync::currEventId);
        DAOSConnectorAsync::resetDAOSEventQueueCounters();

        // Create a new file and write to it.
        objectId += 1;
        KVClientAsync::createObject(objectId);
    }

    // Put into a new key-value pair.
    daos_kv_put(currObjectHandle, DAOS_TX_NONE, 0,
        /* key */ std::to_string(counter).c_str(), size, buf, &_evs[currEventId]);

    // Update the counnters of the DAOS event queue.
    KVClientAsync::acummBytes += size;
    KVClientAsync::currEventId += 1;
}

inline void KVClientAsync::processKVError(const char *op_name, const int err_num)
{
    std::cerr << "Error in DAOS KV object operation " << op_name << ": " << err_num << std::endl;

    int rc = daos_kv_close(currObjectHandle, NULL);  // Close in blocking mode.
    if (rc != 0) {
        KVClientAsync::processError("daos_kv_obj_close", rc);
    }
}

inline void KVClientAsync::printObjectID(const daos_obj_id_t oid) const
{
    std::cout << oid.hi << "." << oid.lo << std::endl;
}

} // namespace ejfat
