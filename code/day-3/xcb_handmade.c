#include <xcb/xcb.h>
#include <malloc.h>

#define INTERNAL        static
#define LOCAL_PERSIST   static
#define GLOBAL_VARIABLE static

GLOBAL_VARIABLE unsigned is_running;
GLOBAL_VARIABLE void*    pixels;

GLOBAL_VARIABLE xcb_connection_t* _connection;
GLOBAL_VARIABLE xcb_screen_t*     _screen;
GLOBAL_VARIABLE xcb_window_t      _window;
GLOBAL_VARIABLE xcb_gcontext_t    _gcontext;
GLOBAL_VARIABLE xcb_pixmap_t      _pixmap;
GLOBAL_VARIABLE xcb_atom_t        _wm_protocols;
GLOBAL_VARIABLE xcb_atom_t        _wm_delete_protocol;

INTERNAL void
unflushed_resize_backbuffer(uint16_t width, uint16_t height) {
   if(pixels) {
      free(pixels);
   }
   if(_pixmap) {
      xcb_free_pixmap(_connection, _pixmap);
   }

   pixels = malloc(width * height * 4);

   //Note(LAG): Code to test if backbuffer has been setup correctly, will color every pixel red
   /*for (int i = 0; i < width * height; i++) {
      ((uint32_t*)pixels)[i] = 0xFF0000;
   }*/

   _pixmap = xcb_generate_id(_connection);
   xcb_create_pixmap(_connection, _screen->root_depth, _pixmap, _window, width, height);
}

INTERNAL void
unflushed_update_window(uint16_t width, uint16_t height) {
   xcb_put_image(_connection,
                 XCB_IMAGE_FORMAT_Z_PIXMAP,
                 _pixmap,
                 _gcontext,
                 width, height,
                 0, 0,
                 0,
                 _screen->root_depth,
                 width * height * 4, (uint8_t*)pixels);
   xcb_copy_area(_connection, _pixmap, _window, _gcontext, 0, 0, 0, 0, width, height);
}

INTERNAL void
handle_event(xcb_generic_event_t* _event) {
   switch(_event->response_type &~0x80) {
      case XCB_CLIENT_MESSAGE:
      {
         xcb_client_message_event_t* _client_message = (xcb_client_message_event_t*)_event;
         if(_client_message->data.data32[0] == _wm_delete_protocol) {
            is_running = 0;
         }
      } break;
      case XCB_CONFIGURE_NOTIFY:
      {
         xcb_configure_notify_event_t* _configure_notify_event = (xcb_configure_notify_event_t*)_event;
         uint16_t width  = _configure_notify_event->width;
         uint16_t height = _configure_notify_event->height;

         unflushed_resize_backbuffer(width, height);
         xcb_flush(_connection);
      } break;
      case XCB_EXPOSE:
      {
         xcb_expose_event_t* _expose_event = (xcb_expose_event_t*)_event;
         uint16_t width = _expose_event->width;
         uint16_t height = _expose_event->height;

         unflushed_update_window(width, height);
         xcb_flush(_connection);
      } break;
   }
   free(_event);
}

int main() {
   _connection = xcb_connect(0, 0);
   if(xcb_connection_has_error(_connection)) {
      //TODO(LAG): Log error
   }

   _screen = xcb_setup_roots_iterator(xcb_get_setup(_connection)).data;

   {
      xcb_intern_atom_cookie_t _intern_atom_cookie;
      xcb_intern_atom_reply_t* _intern_atom_reply;

      _intern_atom_cookie = xcb_intern_atom(_connection, 1, 12, "WM_PROTOCOLS");
      _intern_atom_reply  = xcb_intern_atom_reply(_connection, _intern_atom_cookie, 0);
      _wm_protocols       = _intern_atom_reply->atom;
      free(_intern_atom_reply);

      _intern_atom_cookie = xcb_intern_atom(_connection, 1, 16, "WM_DELETE_WINDOW");
      _intern_atom_reply  = xcb_intern_atom_reply(_connection, _intern_atom_cookie, 0);
      _wm_delete_protocol = _intern_atom_reply->atom;
      free(_intern_atom_reply);
   }

   uint32_t _window_mask = XCB_CW_EVENT_MASK;
   uint32_t _window_values[] = {XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};

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
                     _window_mask, _window_values);
   xcb_change_property(_connection,
                       XCB_PROP_MODE_REPLACE,
                       _window,
                       XCB_ATOM_WM_NAME,
                       XCB_ATOM_STRING,
                       8,
                       8, "Handmade");
   xcb_change_property(_connection,
                       XCB_PROP_MODE_REPLACE,
                       _window,
                       _wm_protocols,
                       XCB_ATOM_ATOM,
                       32,
                       1, &_wm_delete_protocol);
   xcb_map_window(_connection, _window);

   _gcontext = xcb_generate_id(_connection);
   xcb_create_gc(_connection, _gcontext, _screen->root, 0, 0);

   xcb_flush(_connection);

   is_running = 1;
   xcb_generic_event_t* _event;
   while (is_running) {
      while ((_event = xcb_poll_for_event(_connection))) {
         handle_event(_event);
      }
   }

   xcb_disconnect(_connection);
   return 0;
}