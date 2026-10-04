#include "libspatha-icd.c"
#include <xcb/xcb.h>
int main(void){
  xcb_connection_t *c = xcb_connect(NULL,NULL);
  if (xcb_connection_has_error(c)) { puts("sin X"); return 2; }
  xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  uint32_t win = xcb_generate_id(c);
  xcb_create_window(c, scr->root_depth, win, scr->root, 0,0,64,48,0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,0,NULL);
  xcb_map_window(c,win); xcb_flush(c);
  if(!x_load()){ puts("x_load fallo"); return 3; }
  SpathaSurfaceXcb surf = {.connection=c,.window=win};
  struct spatha_swapchain sc; memset(&sc,0,sizeof sc);
  sc.surf=&surf; sc.w=64; sc.h=48; sc.depth=scr->root_depth;
  sc.frame_cap = 8+16+(size_t)64*48*4+64;
  sc.gc = X.generate_id(c); X.create_gc(c,sc.gc,win,0,NULL);
  int ok = shm_setup(&sc);
  printf("shm_setup=%d nshm=%d depth=%u\n", ok, sc.nshm, sc.depth);
  if(!ok) return 4;
  for (int round=0; round<2; round++) {
    uint8_t *px = sc.shm_buf[sc.shm_cur]+24;
    for (int i=0;i<64*48;i++){ px[i*4]=round?0x20:0xC0; px[i*4+1]=0x80; px[i*4+2]=round?0xE0:0x10; px[i*4+3]=0xFF; }
    void *err = X.request_check(c, XS.put_image_checked(c, win, sc.gc, 64,48,0,0,64,48,0,0, sc.depth,2,0, sc.shm_seg[sc.shm_cur], 24));
    printf("round %d put_image err=%s\n", round, err?"SI":"no"); free(err);
    xcb_get_image_reply_t *g = xcb_get_image_reply(c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, win, 0,0,64,48, ~0u), NULL);
    if(!g){ puts("get_image fallo"); return 5; }
    uint8_t *d = xcb_get_image_data(g);
    printf("round %d leido B=%02x G=%02x R=%02x (esperado %02x 80 %02x)\n", round, d[0],d[1],d[2],
           round?0x20:0xC0, round?0xE0:0x10);
    int match = d[0]==(round?0x20:0xC0) && d[1]==0x80 && d[2]==(round?0xE0:0x10);
    free(g); if(!match){ puts("MISMATCH"); return 6; }
    sc.shm_cur ^= 1;
  }
  shm_teardown(&sc);
  puts("OK: SHM end-to-end"); return 0;
}
