#include <xcb/xcb.h>
#include <stdio.h>

int main() {
   xcb_connection_t* _connection = xcb_connect(0, 0);
   if(xcb_connection_has_error(_connection)) {
      printf("Failed to connect\n");
   } else {
      printf("Connection with X Server stablished\n");
   }
   xcb_disconnect(_connection);
}
