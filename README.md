# ejfat
ESnet-JLab FPGA Accelerated Transport

# Using cmake for emulation & simulation executables

#### Check out and build ejfat, ersap-branch:
- mkdir build
- cd build
- cmake ..
- make

##### This will place the compiled executables into build/bin directory

# Using cmake for creating ersap engine/service libraries using ET system

#### Check out & build the ET master branch library:

- setenv CODA \<CODA installtion directory\>
- git clone https://github.com/JeffersonLab/et.git
- cd et
- mkdir build
- cd build
- cmake ..
- make install

###### This will place the libraries into $CODA/\<arch\>/lib

#### Check out and build ejfat, ersap-branch with flags:

- setenv ERSAP_HOME \<ERSAP installtion directory\>
- mkdir build
- cd build
- cmake .. -DBUILD_ERSAP=1 -DBUILD_ET=1 -DBUILD_ZMQ=1
- make install

###### This will place the compiled executables into build/bin directory and the engine/services libraries into ERSAP_HOME

# Using the ET system as a FIFO for ERSAP backend

#### Create and run the ET system for ejfat reassembler by calling

- et_start_fifo -v -f \<filename\> -s \<event size\> -n \<events/fifo-entry\> -e \<fifo-entries\>

##### Use -h flag to see more options. The events/fifo  >= number of data sources.

#### In ERSAP reassembly service/engine, do the following:

- open ET system
- create fifo object
- use fifo object interact with events (get, put, etc)
- close fifo and ET when finished

#### Java code to access data in ET will look something like the following (see FifoConsumer.java for details):

        try {
            // Use config object to specify how to open ET system
            EtSystemOpenConfig config = new EtSystemOpenConfig();

            // Create ET system object with verbose debugging output
            EtSystem sys = new EtSystem(config);
            if (verbose) {
                sys.setDebug(EtConstants.debugInfo);
            }
            sys.open();

            //------------------------
            // Use FIFO interface
            // (takes care of attaching to proper station, etc.)
            //------------------------
            EtFifo fifo = new EtFifo(sys);

            EtFifoEntry entry = new EtFifoEntry(sys, fifo);

            // Max number of events per fifo entry
            int entryCap = fifo.getEntryCapacity();
            //------------------------

            // Array of events
            EtEvent[] mevs;
            int idCount, bufId, len;

            while (true) {
                //----------------------------
                // Get events from ET system
                //----------------------------
                fifo.getEntry(entry);
                mevs = entry.getBuffers();

                idCount = 0;

                // Go through each event and do something with it
                for (int i=0; i < entryCap; i++) {
                    // Does this buffer have any data? (Set by producer). If not ...
                    if (!mevs[i].hasFifoData()) {
                        // Once we hit a buffer with no data, there is no further data
                        break;
                    }
                    idCount++;

                    // Source Id associated with this buffer in this fifo entry
                    bufId = mevs[i].getFifoId();

                    // Get event's data buffer
                    ByteBuffer buf = mevs[i].getDataBuffer();
                    
                    // Or get the array backing the ByteBuffer
                    byte[] data = mevs[i].getData();
                    
                   // Length of valid data
                   len = mevs[i].getLength();
                }

               //----------------------------
               // Put events back into ET system
               //----------------------------
               fifo.putEntry(entry);
            }
        
            fifo.close();
            sys.close();
        
        }
        catch (Exception ex) {
            ex.printStackTrace();
        }

#### C code to access data in ET will look something like the following (see et_fifoConsumer.c for details):

    et_sys_id       id;
    et_fifo_id      fid;
    et_fifo_entry   *entry;
    et_openconfig   openconfig;
    char            et_name[ET_FILENAME_LENGTH];
  
    et_open_config_init(&openconfig);

    if (et_open(&id, et_name, openconfig) != ET_OK) {
        printf("et_open problems\n");
        exit(1);
    }

    /* set level of debug output (everything) */
    et_system_setdebug(id, debugLevel);

    //-----------------------
    // Use FIFO interface
    // (takes care of attaching to proper station, etc.)
    //-----------------------
    int status = et_fifo_openConsumer(id, &fid);
    if (status != ET_OK) {
        printf("et_fifo_open problems\n");
        exit(1);
    }
    
    // Max number of buffers per fifo entry
    numRead = et_fifo_getEntryCapacity(fid);

    // Create a place to store fifo entry
    entry = et_fifo_entryCreate(fid);
    if (entry == NULL) {
        printf("et_fifo_open out of mem\n");
        exit(1);
    }

    int bufId, hasData, swap;
    size_t len;
    int *data;
    
    while (1) {
        //-----------------
        // get events
        //-----------------
    
        // Get fifo entry
        status = et_fifo_getEntry(fid, entry);
        if (status != ET_OK) {
            printf("error getting events\n");
            goto error;
        }
    
        // Access the new buffers
        et_event** evts = et_fifo_getBufs(entry);
    
        int idCount = 0;

        // Look at each event/buffer
        for (j = 0; j < numRead; j++) {
            // Does this buffer have any data? (Set by producer). If not ...
            if (!et_fifo_hasData(evts[j])) {
                // Once we hit a buffer with no data, there is no further data
                break;
            }
            idCount++;

            // Source Id associated with this buffer in this fifo entry
            bufId = et_fifo_getId(evts[j]);

            // Data associated with this event
            et_event_getdata(evts[j], (void **) &data);
            
            // Length of data associated with this event
            et_event_getlength(evts[j], &len);
            
            // Did this data originate on an opposite endian machine?
            et_event_needtoswap(evts[j], &swap);
        }

        //-----------------
        // put events
        //-----------------

        // Putting array of events
        status = et_fifo_putEntry(entry);
        if (status != ET_OK) {
            printf("error getting events\n");
            goto error;
        }

    } /* while(1) */

    error:
        et_fifo_freeEntry(entry);
        et_fifo_close(fid);
        et_close(id);
