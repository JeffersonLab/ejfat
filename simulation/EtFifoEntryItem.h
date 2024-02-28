//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_ETFIFOENTRYITEM_H
#define UTIL_ETFIFOENTRYITEM_H


#include <memory>
#include <atomic>
#include <functional>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "SupplyItem.h"
#include "ejfat_assemble_ersap.hpp"

#include "et.h"
#include "et_fifo.h"


#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif


namespace ejfat {

    /**
     * This class defines an ET system's FIFO entry and associated qunatities
     * which are supplied by the Supplier class.
     *
     * @date 02/27/2024
     * @author timmer
     */
    class EtFifoEntryItem : public SupplyItem {

    public:

        /** ET system id. */
        static et_fifo_id fid;

    private:

        /** ET fifo entry structure pointer. */
        et_fifo_entry *entry;

    public:

        static void setEventFactorySettings(et_fifo_id id);
        static const std::function< std::shared_ptr<EtFifoEntryItem> () >& eventFactory();

        EtFifoEntryItem();
        EtFifoEntryItem(const EtFifoEntryItem & item);
        ~EtFifoEntryItem();

        EtFifoEntryItem & operator=(const EtFifoEntryItem & other) = delete;

        et_fifo_entry* getEntry();

        void setEntry(et_fifo_entry *entry);

        void reset();
    };
}


#endif
