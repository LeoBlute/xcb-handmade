#include <xcb/xcb.h>
#include <malloc.h>

static xcb_connection_t* _connection;
static xcb_window_t      _window;

void
handle_event(xcb_generic_event_t* _event) {
   switch(_event->response_type &~0x80) {
      case XCB_EXPOSE:
      {
         static unsigned is_white = 0;
         uint32_t color = is_white ? 0xFFFFFF : 0x000000;
         xcb_change_window_attributes(_connection, _window, XCB_CW_BACK_PIXEL, (uint32_t[]){color});
         xcb_flush(_connection);

         is_white = !is_white;
      }
      break;
   }
   free(_event);
}

int main() {
   _connection = xcb_connect(0, 0);
   if(xcb_connection_has_error(_connection)) {
      //TODO(LAG): Log error
   }

   xcb_screen_t* _screen = xcb_screen_t* _screen = xcb_setup_roots_iterator(xcb_get_setup(_connection)).data;;

   uint32_t _mask = XCB_CW_EVENT_MASK;
   uint32_t _values[] = {XCB_EVENT_MASK_EXPOSURE};

   _window = xcb_generate_id(_connection);
   xcb_create_window(_connection,
                     XCB_COPY_FROM_PARENT,
                     _window,
                     _screen->root,
                     0, 0,
                     1280, 720,
                     0,
                     XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     _screen->root_visual,
                     _mask, _values);
   xcb_change_property(_connection,
                       XCB_PROP_MODE_REPLACE,
                       _window,
                       XCB_ATOM_WM_NAME,
                       XCB_ATOM_STRING,
                       8,
                       8, "Handmade");
   xcb_map_window(_connection, _window);

   xcb_flush(_connection);

   xcb_generic_event_t* _event;
   while ((_event = xcb_wait_for_event(_connection))) {
      handle_event(_event);
   }

   xcb_disconnect(_connection);
   return 0;
}
