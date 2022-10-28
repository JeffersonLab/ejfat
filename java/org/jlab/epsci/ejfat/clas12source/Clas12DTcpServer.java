/*
 * Copyright (c) 2014, Jefferson Science Associates
 *
 * Thomas Jefferson National Accelerator Facility
 * Data Acquisition Group
 *
 * 12000, Jefferson Ave, Newport News, VA 23606
 * Phone : (757)-269-7100
 *
 */

package org.jlab.epsci.ejfat.clas12source;


import java.io.*;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.util.HashMap;
import java.util.Iterator;
import java.nio.ByteOrder;


class debugConstants {
    public static final int version = 6;
    public static final int debugNone = 0;
    public static final int debugSevere = 1;
    public static final int debugError = 2;
    public static final int debugWarn = 3;
    public static final int debugInfo = 4;
}


/**
 * This class is the cMsg Emu domain TCP server run inside of an EMU.
 * It accepts connections from ROCs and SEBs. Its purpose is to implement
 * fast, efficient communication between those components and EBs and ERs.
 *
 * @author timmer (4/18/14)
 */
public class Clas12DTcpServer extends Thread {


    /** Level of debug output for this class. */
    private final int debug = debugConstants.debugError;

    static int serverPort = 8989;

    /** Setting this to true will kill all threads. */
    private volatile boolean killThreads;

    /** Size of buffer in bytes in which to receive incoming data. */
    final static int MAX_BUF_BYTE_SIZE = 4000000;



    /** Buffer in which to read incoming data. */
    byte[] data = new byte[MAX_BUF_BYTE_SIZE];
    ByteBuffer buf = ByteBuffer.wrap(data);



    /** Kills this and all spawned threads. */
    void killAllThreads() {
        killThreads = true;
        this.interrupt();
    }


//    /**
//     * Turn section of byte array into an int.
//     *
//     * @param data byte array to convert
//     * @param byteOrder byte order of supplied bytes (big endian if null)
//     * @param off offset into data array
//     * @return int converted from byte array
//     * @throws EvioException if data is null or wrong size, or off is negative
//     */
//    public static int toInt(byte[] data, ByteOrder byteOrder, int off) throws Exception {
//        if (data == null || data.length < 4+off || off < 0) {
//            throw new Exception("bad data arg");
//        }
//
//        if (byteOrder == null || byteOrder == ByteOrder.BIG_ENDIAN) {
//            return (
//                    (0xff & data[  off]) << 24 |
//                            (0xff & data[1+off]) << 16 |
//                            (0xff & data[2+off]) <<  8 |
//                            (0xff & data[3+off])
//            );
//        }
//        else {
//            return (
//                    (0xff & data[  off])       |
//                            (0xff & data[1+off]) <<  8 |
//                            (0xff & data[2+off]) << 16 |
//                            (0xff & data[3+off]) << 24
//            );
//        }
//    }

    /**
     * Turn 4 bytes into an int.
     *
     * @param b1 1st byte
     * @param b2 2nd byte
     * @param b3 3rd byte
     * @param b4 4th byte
     * @param byteOrder if big endian, 1st byte is most significant &amp;
     *                  4th is least (big endian if null)
     * @return int converted from byte array
     */
    public static int toInt(byte b1, byte b2, byte b3, byte b4, ByteOrder byteOrder) {

        if (byteOrder == null || byteOrder == ByteOrder.BIG_ENDIAN) {
            return (
                    (0xff & b1) << 24 |
                            (0xff & b2) << 16 |
                            (0xff & b3) <<  8 |
                            (0xff & b4)
            );
        }
        else {
            return (
                    (0xff & b1)       |
                            (0xff & b2) <<  8 |
                            (0xff & b3) << 16 |
                            (0xff & b4) << 24
            );
        }
    }


    /**
     * Run as a stand-alone application.
     */
    public static void main(String[] args) {
        Clas12DTcpServer server = new Clas12DTcpServer(serverPort);
        server.run();
    }


    /**
     * Constructor.
     * @param server emu server that created this object
     * @param port TCP port on which to receive transmissions from emu clients
     */
    public Clas12DTcpServer(int port) {
        setName("ERSAP Simulated TCP server");
        serverPort = port;
    }


    /** This method is executed as a thread. */
    public void run() {
        if (debug >= debugConstants.debugInfo) {
            System.out.println("ERSAP TCP Server: running, listening on port " + serverPort);
        }

        Selector selector = null;
        ServerSocketChannel serverChannel = null;

        try {
            // Get things ready for a select call
            selector = Selector.open();

            // Bind to the given TCP listening port. If not possible, throw exception
            try {
                serverChannel = ServerSocketChannel.open();
                ServerSocket listeningSocket = serverChannel.socket();
                listeningSocket.setReuseAddress(true);
                // We prefer high bandwidth over low latency & short connection times
                listeningSocket.setPerformancePreferences(0,0,1);
                listeningSocket.bind(new InetSocketAddress(serverPort));
            }
            catch (IOException ex) {
                System.out.println("    Emu TCP Server: TCP port number " + serverPort + " already in use.");
                System.exit(-1);
            }

            // Set non-blocking mode for the listening socket
            serverChannel.configureBlocking(false);

            // Register the channel with the selector for accepts
            serverChannel.register(selector, SelectionKey.OP_ACCEPT);

            // EmuDomainServer object is waiting for this thread to start, so tell it we've started.
            synchronized (this) {
                notifyAll();
            }

            while (true) {
                // 3 second timeout
                int n = selector.select(3000);

                // If no channels (sockets) are ready, listen some more
                if (n == 0) {
                    // But first check to see if we've been commanded to die
                    if (killThreads) {
                        System.out.println("    Emu TCP Server: thread exiting");
                        return;
                    }
                    continue;
                }
//System.out.println("    Emu TCP Server: someone trying to connect");

                // Get an iterator of selected keys (ready sockets)
                Iterator it = selector.selectedKeys().iterator();

                // Look at each key
                keyLoop:
                while (it.hasNext()) {
                    SelectionKey key = (SelectionKey) it.next();

                    // Is this a new connection coming in?
                    if (key.isValid() && key.isAcceptable()) {

                        // Accept the connection from the client
                        SocketChannel channel = serverChannel.accept();

                        // Go back to using streams
                        channel.configureBlocking(true);
                        readBuffers(channel);
                    }

                    // remove key from selected set since it's been handled
                    it.remove();
                }
            }
        }
        catch (Exception ex) {
            ex.printStackTrace();
        }
        finally {
            try {if (serverChannel != null) serverChannel.close();} catch (IOException e) {}
            try {if (selector != null) selector.close();} catch (IOException e) {}
        }

        if (debug >= debugConstants.debugInfo) {
            System.out.println("    ERSAP TCP Server: quitting");
        }
    }



    private void readBuffers(SocketChannel channel) {
        try {
            while (true) {

                // Read incoming buffer size in bytes, little endian
                buf.clear();
                buf.limit(4);
                System.out.println("    ERSAP TCP Server: read size");
                int bytes = channel.read(buf);

                int bufferSize = toInt(data[0], data[1], data[2], data[3], ByteOrder.LITTLE_ENDIAN);

                System.out.println("    ERSAP TCP Server: size = " + bufferSize);

                buf.clear();
                buf.limit(bufferSize);
                int bytesRead = channel.read(buf);

                System.out.println("    ERSAP TCP Server: read whole buffer, " + bytesRead + " bytes");
            }
        }
        catch (java.lang.Exception e) {
            e.printStackTrace();
        }

    }

}
