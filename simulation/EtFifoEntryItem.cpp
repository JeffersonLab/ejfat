//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "EtFifoEntryItem.h"


namespace ejfat {



    //--------------------------------
    // STATIC INITIALIZATION
    //--------------------------------

    void * EtFifoEntryItem::fid {nullptr};

    /**
     * Method to set EtFifoEntryItem parameters for objects created by eventFactory.
     * Doing things in this roundabout manor is necessary because the disruptor's
     * createSingleProducer method takes a function for created items which has no args! Thus these args,
     * needed for construction of each BufferItem, must be passed in as global parameters.
     *
     * @param id ET fifo API's id.
     */
    void EtFifoEntryItem::setEventFactorySettings(et_fifo_id id) {
        EtFifoEntryItem::fid = id;
    }


    /** Function to create EtFifoEntryItems by RingBuffer. */
    const std::function< std::shared_ptr<EtFifoEntryItem> () >& EtFifoEntryItem::eventFactory() {
        static std::function< std::shared_ptr<EtFifoEntryItem> () > result([]  {
            return std::make_shared<EtFifoEntryItem>();
        });
        return result;
    }


    /** Default constructor. */
    EtFifoEntryItem::EtFifoEntryItem() : SupplyItem() {
        //myId = idValue++;
        entry = et_fifo_entryCreate(EtFifoEntryItem::fid);
        if (entry == NULL) {
            fprintf(stderr, "et_fifo_entryCreate: out of mem\n");
            exit(1);
        }
    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    EtFifoEntryItem::EtFifoEntryItem(const EtFifoEntryItem & item) : SupplyItem(item) {
        // Avoid self copy ...
        if (this != &item) {
            entry = item.entry;
        }
    }


    /** Destructor. */
    EtFifoEntryItem::~EtFifoEntryItem() {
        et_fifo_freeEntry(entry);
    }


    /** Method to reset this item each time it is retrieved from the supply. */
    void EtFifoEntryItem::reset() {
        SupplyItem::reset();
    }


    /**
     * Get pointer to ET fifo entry.
     * @return pointer to ET fifo entry.
     */
    et_fifo_entry* EtFifoEntryItem::getEntry() {return entry;}

    /**
     * Set the ET fifo entry.
     * @param ent ET fifo entry.
     */
    void EtFifoEntryItem::setEntry(et_fifo_entry *ent) {entry = ent;}


}
