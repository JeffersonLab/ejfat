#### clasBlaster.cc

This is a general data-sending program which reads events from a
CLAS data file in the hipo format and sends each hipo event as an
ejfat event to the given destination. Many command line options.

To compile this, cmake must be given the arg, -DBUILD_CLAS=1.


**Dependencies:**
1) the **hipo** lib


