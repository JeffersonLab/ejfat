// Client-Side Program 
// C program to demonstrate socket programming
// as well as explicitly assigning a port number
// on Client Side
   
#include<stdio.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<sys/un.h>
#include<string.h>
#include<netdb.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<stdlib.h>
   
int main()
   {
   // Two buffer are for message communication
   char buffer1[256], buffer2[256];
   struct sockaddr_in my_addr, my_addr1;
   int client = socket(AF_INET, SOCK_STREAM, 0);
   if (client < 0)
      printf("Error in client creating\n");
   else
      printf("Client Created\n");
   my_addr.sin_family = AF_INET;
   my_addr.sin_addr.s_addr = INADDR_ANY;
   my_addr.sin_port = htons(12000);
   
   // This ip address will change according to the machine
   
   my_addr.sin_addr.s_addr = inet_addr("129.57.29.231"); // dest addrr ????
   
   // Explicitly assigning port number 12010 by
   // binding client with that port
   
   my_addr1.sin_family = AF_INET;
   my_addr1.sin_addr.s_addr = INADDR_ANY;
   my_addr1.sin_port = htons(12010);
   
   // This ip address will change according to the machine
   
   my_addr1.sin_addr.s_addr = inet_addr("129.57.29.230"); //egress ???
   if (bind(client, (struct sockaddr*) &my_addr1, sizeof(struct sockaddr_in)) == 0)
      printf("Binded Correctly\n");
   else
      printf("Unable to bind\n");
   
   socklen_t addr_size = sizeof my_addr;
   int con = connect(client, (struct sockaddr*) &my_addr, sizeof my_addr);
   if (con == 0)
      printf("Client Connected\n");
   else
      printf("Error in Connection\n");
   
   strcpy(buffer2, "Hello");
   send(client, buffer2, 256, 0);
   recv(client, buffer1, 256, 0);
   printf("Server : %s\n", buffer1);
   return 0;
}
