#if !defined(XCB_HANDMADE)
#define XCB_HANDMADE

typedef struct xxcb_offscreen_buffer {
   xcb_pixmap_t pixmap;
   void*        pixels;
   u16          width;
   u16          height;
   u16          pitch;
} xxcb_offscreen_buffer;

typedef struct alsa_sound_output {
   int samples_per_second;
   int samples_per_write;
   int bytes_per_sample;
   int tone_hz;
} alsa_sound_output;

#endif
