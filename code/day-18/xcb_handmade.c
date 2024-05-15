#include <xcb/xcb.h>
#include <xcb/xkb.h> /*Require libxcb-xkb-dev package installed*/
#include <alsa/asoundlib.h>
#include <linux/joystick.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>
#include <x86gprintrin.h>

#define PI32 3.14159265359f

#define INTERNAL        static
#define LOCAL_PERSIST   static
#define GLOBAL_VARIABLE static

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef float  f32;
typedef double f64;

typedef u32 bool32;

#define FALSE 0
#define TRUE  1

#include "handmade.h"
#include "handmade.c"

#include "xcb_handmade.h"

#define BYTES_PER_PIXEL 4

GLOBAL_VARIABLE unsigned              is_running;
GLOBAL_VARIABLE xxcb_offscreen_buffer global_backbuffer;
GLOBAL_VARIABLE unsigned              keys_down[200];

GLOBAL_VARIABLE xcb_connection_t* _connection;
GLOBAL_VARIABLE xcb_screen_t*     _screen;
GLOBAL_VARIABLE xcb_window_t      _window;
GLOBAL_VARIABLE xcb_gcontext_t    _gcontext;
GLOBAL_VARIABLE xcb_atom_t        _wm_protocols;
GLOBAL_VARIABLE xcb_atom_t        _wm_delete_protocol;

GLOBAL_VARIABLE snd_pcm_t* _pcm;

//Note(LAG): Do not test with __FILE__
INTERNAL debug_read_file_result debug_platform_read_entire_file(char* filename) {
   debug_read_file_result result = {};

   int file_handle = open(filename, O_RDONLY);
   if(file_handle == -1) {
      return result;
   }

   struct stat file_status;
   if(fstat(file_handle, &file_status) == -1) {
      close(file_handle);
      return result;
   }
   result.content_size = safe_truncate_u64(file_status.st_size);
   result.contents = malloc(result.content_size);
   if(!result.contents) {
      close(file_handle);
      result.content_size = 0;
      return result;
   }

   u32 bytes_to_read = result.content_size;
   u8* next_byte_location = (u8*)result.contents;
   while(bytes_to_read) {
      u32 bytes_read = read(file_handle, next_byte_location, bytes_to_read);
      if(bytes_read == -1) {
         free(result.contents);
         result.contents = 0;
         result.content_size = 0;
         close(file_handle);
         return result;
      }
      bytes_to_read -= bytes_read;
      next_byte_location += bytes_read;
   }

   close(file_handle);
   return result;
}
INTERNAL void debug_platform_free_file_memory(void* memory) {
   free(memory);
}
INTERNAL bool32 debug_platform_write_entire_file(char* filename, u32 memory_size, void* memory) {
   int file_handle = open(filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

   if(!file_handle) {
      return FALSE;
   }

   u32 bytes_to_write = memory_size;
   u8* next_byte_location = (u8*)memory;

   while(bytes_to_write) {
      u32 bytes_written = write(file_handle, next_byte_location, bytes_to_write);
      if(bytes_written == -1) {
         close(file_handle);
         return FALSE;
      }

      bytes_to_write -= bytes_written;
      next_byte_location += bytes_written;
   }

   close(file_handle);
   return TRUE;
}

INTERNAL void
alsa_fill_sound_buffer(game_sound_output_buffer* sound_buffer) {
   int result;
   while((result = snd_pcm_writei(_pcm, sound_buffer->samples, sound_buffer->sample_count)) != sound_buffer->sample_count) {
      if(result == -EPIPE) {
         snd_pcm_prepare(_pcm);
      } else {
         //TODO(LAG): Diagnostic
      }
   }  
}

INTERNAL void
alsa_init(int samples_per_second, int samples_per_write) {
   snd_pcm_hw_params_t* _pcm_hw_params;

   if(snd_pcm_open(&_pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
      //TODO(LAG) Diagnostic
   }

   _pcm_hw_params = (snd_pcm_hw_params_t*)mmap(0,
                                               snd_pcm_hw_params_sizeof(),
                                               PROT_READ | PROT_WRITE,
                                               MAP_PRIVATE | MAP_ANONYMOUS,
                                               -1,
                                               0);
   snd_pcm_hw_params_any(_pcm, _pcm_hw_params);

   snd_pcm_hw_params_set_access(_pcm, _pcm_hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
   snd_pcm_hw_params_set_format(_pcm, _pcm_hw_params, SND_PCM_FORMAT_S16_LE);
   snd_pcm_hw_params_set_channels(_pcm, _pcm_hw_params, 2);
   snd_pcm_hw_params_set_rate(_pcm, _pcm_hw_params, samples_per_second, 0);
   snd_pcm_hw_params_set_buffer_size(_pcm, _pcm_hw_params, samples_per_write);
   snd_pcm_hw_params_set_period_time(_pcm, _pcm_hw_params, 100000, 0);

   snd_pcm_hw_params(_pcm, _pcm_hw_params);
}

INTERNAL void
unflushed_resize_backbuffer(xxcb_offscreen_buffer* buffer, u16 width, u16 height) {
   if(buffer->pixels) {
      munmap(buffer->pixels, buffer->width * buffer->height * BYTES_PER_PIXEL);
   }
   if(buffer->pixmap) {
      xcb_free_pixmap(_connection, buffer->pixmap);
   }

   buffer->width  = width;
   buffer->height = height;
   buffer->pitch  = width * BYTES_PER_PIXEL;
   buffer->pixels = mmap(0,
                         width * height * BYTES_PER_PIXEL,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1,
                         0);

   buffer->pixmap = xcb_generate_id(_connection);
   xcb_create_pixmap(_connection, _screen->root_depth, buffer->pixmap, _window, width, height);
}

INTERNAL void
unflushed_update_window(xxcb_offscreen_buffer buffer, u16 width, u16 height) {
   xcb_put_image(_connection,
                 XCB_IMAGE_FORMAT_Z_PIXMAP,
                 buffer.pixmap,
                 _gcontext,
                 width, height,
                 0, 0,
                 0,
                 _screen->root_depth,
                 width * height * BYTES_PER_PIXEL, (u8*)buffer.pixels);
   xcb_copy_area(_connection, buffer.pixmap, _window, _gcontext, 0, 0, 0, 0, width, height);
}

INTERNAL void
joystick_input_process(game_button_state* old_state, game_button_state* new_state, s16 value) {
   new_state->ended_down = value;
   new_state->half_transition_count = (old_state->ended_down != new_state->ended_down) ? 1 : 0;
}

INTERNAL f32
joystick_axis_process(s16 value) {
   s16 dead_zone = 7849;
   f32 axis_limit = 32767.0f;
   if(abs(value) < dead_zone) {
      return 0;
   } else if(value < -dead_zone) {
      return (f32)((value + dead_zone) / (axis_limit - dead_zone));
   }
   return (f32)((value - dead_zone) / (axis_limit - dead_zone));
}

INTERNAL void
keyboard_input_process(game_button_state* new_state, bool32 is_down) {
   new_state->ended_down = is_down;
   ++new_state->half_transition_count;
}

INTERNAL struct timeval
get_timeval(void) {
   struct timeval tv;
   gettimeofday(&tv, 0);
   return tv;
}

INTERNAL f32
get_seconds_elapsed(struct timeval tv_start, struct timeval tv_end) {
   return (f32)(((tv_end.tv_sec * 1000000) + tv_end.tv_usec) - ((tv_start.tv_sec * 1000000) + tv_start.tv_usec)) / (1000.0f*1000.0f);
}

int main() {
   _connection = xcb_connect(0, 0);
   if(xcb_connection_has_error(_connection)) {
      //TODO(LAG): Log error
   }

   _screen = xcb_setup_roots_iterator(xcb_get_setup(_connection)).data;

   {
      xcb_xkb_use_extension(_connection, 
                            XCB_XKB_MAJOR_VERSION, 
                            XCB_XKB_MINOR_VERSION);
      xcb_xkb_per_client_flags(_connection,
                               XCB_XKB_ID_USE_CORE_KBD,                                                       
                               XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
                               1,0,0,0);
   }

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

   int monitor_refresh_hz = 60;
   int game_update_hz = monitor_refresh_hz / 2;
   f32 target_seconds_per_frame = 1.0f / (f32)game_update_hz;

   u32 _window_mask = XCB_CW_EVENT_MASK;
   u32 _window_values[] = {XCB_EVENT_MASK_EXPOSURE         |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_KEY_PRESS        |
                           XCB_EVENT_MASK_KEY_RELEASE};

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

   int _joystick_descriptor = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
   if(_joystick_descriptor < 0) {
      //TODO(LAG): Handle Gamepad connected
   } else {
      //TODO(LAG): Handle Gamepad not found
   }

   alsa_sound_output sound_output = {};

   sound_output.samples_per_second = 48000;
   sound_output.samples_per_write  = sound_output.samples_per_second/15;
   sound_output.bytes_per_sample = 2 * sizeof(s16);
   sound_output.tone_hz = 256;

   alsa_init(sound_output.samples_per_second, sound_output.samples_per_write);

   struct timeval last_counter = get_timeval();

   u64 last_cycle_count = __rdtsc();

   is_running  = 1;

   s16* samples = mmap(0,
                       sound_output.samples_per_write * sound_output.bytes_per_sample,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1,
                       0);

   {
      game_sound_output_buffer sound_buffer = {};
      sound_buffer.samples_per_second = sound_output.samples_per_second;
      sound_buffer.sample_count = sound_output.samples_per_write;
      sound_buffer.samples = samples;
      s16* sample_out = samples;
      for(int i=0; i<sound_buffer.sample_count; ++i) {
         *sample_out++ = 0;
         *sample_out++ = 0;
      }
      alsa_fill_sound_buffer(&sound_buffer);
   }

   game_input input[2] = {};
   game_input* new_input = &input[0];
   game_input* old_input = &input[1];

   game_memory gmemory = {};
   gmemory.permanent_storage_size = MEGABYTES(64);
   gmemory.transient_storage_size = GIGABYTES(1);
   u64 total_size = gmemory.permanent_storage_size + gmemory.transient_storage_size;
   gmemory.permanent_storage = mmap(0,
                                    total_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS,
                                    -1,
                                    0);
   gmemory.transient_storage = ((u8*)gmemory.permanent_storage + gmemory.permanent_storage_size);

   if(!samples || !gmemory.permanent_storage || !gmemory.transient_storage) {
      return 1;
   }

   while (is_running) {
      xcb_generic_event_t* _event;
      struct js_event      _joystick_event;

      game_controller_input* old_controller = &old_input->controllers[0];
      game_controller_input* new_controller = &new_input->controllers[0];
      game_controller_input* new_keyboard_controller = &new_input->controllers[0];
      game_controller_input* old_keyboard_controller = &old_input->controllers[0];

      game_controller_input zero_controller = {};
      *new_keyboard_controller = zero_controller;

      for(int button_index=0; button_index < ARRAY_COUNT(new_keyboard_controller->buttons); ++button_index) {
         new_keyboard_controller->buttons[button_index].ended_down = old_keyboard_controller->buttons[button_index].ended_down;
      }


      while(read(_joystick_descriptor, &_joystick_event, sizeof(struct js_event)) == sizeof(struct js_event)) {
         new_controller->is_analog = TRUE;
         _joystick_event.type &= ~JS_EVENT_INIT;
         switch(_joystick_event.type) {
            case JS_EVENT_BUTTON:
            {
               s16 button_value = _joystick_event.value;
               u8  button_code  = _joystick_event.number;
               if(button_code == 0) {
                  joystick_input_process(&old_controller->action_down,  &new_controller->action_down,  button_value);
               } else 
               if (button_code == 1){
                  joystick_input_process(&old_controller->action_up,    &new_controller->action_up,    button_value);
               } else 
               if (button_code == 2){
                  joystick_input_process(&old_controller->action_right, &new_controller->action_right, button_value);
               } else 
               if (button_code == 3){
                  joystick_input_process(&old_controller->action_left,  &new_controller->action_left,  button_value);
               } else
               if (button_code == 6) {
                  joystick_input_process(&old_controller->back,         &new_controller->back,         button_value);
               } else
               if (button_code == 7) {
                  joystick_input_process(&old_controller->start,        &new_controller->start,        button_value);
               }

            } break;
            case JS_EVENT_AXIS:
            {
               f32 norm_lx = 0.0f;
               f32 norm_ly = 0.0f;
               s16 stick_value = _joystick_event.value;
               u8  stick_code  = _joystick_event.number;
               printf("keycode:%d\n", stick_code);
               if(stick_code == 0) {
                  norm_lx = joystick_axis_process(stick_value);
               } else
               if(stick_code == 1) {
                  norm_ly = joystick_axis_process(stick_value);
               }

               new_controller->stick_averagex = norm_lx;
               new_controller->stick_averagey = norm_ly;

               f32 threshold = 0.5f;
               joystick_input_process(&old_controller->move_up, &new_controller->move_up, (new_controller->stick_averagey < -threshold));
               joystick_input_process(&old_controller->move_down, &new_controller->move_down, (new_controller->stick_averagey > threshold));
               joystick_input_process(&old_controller->move_right, &new_controller->move_right, (new_controller->stick_averagex > threshold));
               joystick_input_process(&old_controller->move_left, &new_controller->move_left, (new_controller->stick_averagex < -threshold));
            } break;
         }
      }

      while ((_event = xcb_poll_for_event(_connection))) {
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
               u16 width  = _configure_notify_event->width;
               u16 height = _configure_notify_event->height;

               unflushed_resize_backbuffer(&global_backbuffer, width, height);
               xcb_flush(_connection);
            } break;
            case XCB_EXPOSE:
            {
               xcb_expose_event_t* _expose_event = (xcb_expose_event_t*)_event;
               u16 width = _expose_event->width;
               u16 height = _expose_event->height;
            } break;
            case XCB_KEY_RELEASE:
            case XCB_KEY_PRESS:
            {
               xcb_key_press_event_t* _key_event = (xcb_key_press_event_t*)_event;
               u8 keycode = _key_event->detail;
               bool32 is_down = (_event->response_type &~0x80) == XCB_KEY_PRESS ? TRUE : FALSE;

               if((_event->response_type &~0x80) == XCB_KEY_PRESS && !keys_down[keycode]) {
                  keys_down[keycode] = 1;
               } else if((_event->response_type &~0x80) == XCB_KEY_RELEASE) {
                  keys_down[keycode] = 0;
               }
               if(keycode == 111) {
                  keyboard_input_process(&new_controller->action_up,    is_down);
               } else if(keycode == 116) {
                  keyboard_input_process(&new_controller->action_down,  is_down);
               } else if(keycode == 113) {
                  keyboard_input_process(&new_controller->action_left,  is_down);
               } else if(keycode == 114) {
                  keyboard_input_process(&new_controller->action_right, is_down);
               }
            } break;
         }
         free(_event);
      }

      snd_pcm_sframes_t delay;
      snd_pcm_delay(_pcm, &delay);
      int samples_to_write = sound_output.samples_per_write - delay;

      game_sound_output_buffer sound_buffer = {};
      sound_buffer.samples_per_second = sound_output.samples_per_second;
      sound_buffer.sample_count = samples_to_write;
      sound_buffer.samples = samples;

      game_offscreen_buffer buffer = {};
      buffer.memory = global_backbuffer.pixels;
      buffer.width = global_backbuffer.width;
      buffer.height = global_backbuffer.height;
      buffer.pitch = global_backbuffer.pitch;
      game_update_render(&gmemory, new_input, &buffer, &sound_buffer);

      if(samples_to_write > 0) {
         alsa_fill_sound_buffer(&sound_buffer);
      }

      struct timeval work_counter = get_timeval();

      f32 work_seconds_elapsed = get_seconds_elapsed(last_counter, work_counter);
      f32 seconds_elapsed_for_frame = work_seconds_elapsed;

      if(seconds_elapsed_for_frame < target_seconds_per_frame) {
         //Note(LAG) Due to granularity, it cannot hit the right amount of sleep time so we make it sleep for a little less time than what it should and the loop handle the rest
         s32 sleep_usecs = (s32)((1000.0f*980.0f) * (target_seconds_per_frame - seconds_elapsed_for_frame));
         if(sleep_usecs > 0) {
            usleep(sleep_usecs);
         }

         while(seconds_elapsed_for_frame < target_seconds_per_frame) {
            seconds_elapsed_for_frame = get_seconds_elapsed(last_counter, get_timeval());
         }
      } else {
         //TODO(Casey): MISSED FRAME RATE!
         //TODO(Casey): Logging
      }

      struct timeval end_counter = get_timeval();
      s64 end_cycle_count = __rdtsc();

      u16 width  = global_backbuffer.width;
      u16 height = global_backbuffer.height;
      unflushed_update_window(global_backbuffer, width, height);
      xcb_flush(_connection);

      game_input* temp = new_input;
      new_input = old_input;
      old_input = temp;

#if 0
      u64 cycles_elapsed = end_cycle_count - last_cycle_count;

      f32 ms_per_frame = 1000.0f * get_seconds_elapsed(last_counter, end_counter);
      f32 fps = (1000.0f) / ms_per_frame;
      f32 mcpf = (f32)(cycles_elapsed / (1000.0f*1000.0f));

      char char_buffer[256];
      int length;
      length = sprintf(char_buffer, "%.2fms/f, %.2ff/s, %.2fmc/f\n", ms_per_frame, fps, mcpf);
      write(STDOUT_FILENO, char_buffer, length);
#endif

      last_counter = end_counter;
      last_cycle_count = end_cycle_count;
   }

   xcb_disconnect(_connection);
   return 0;
}
