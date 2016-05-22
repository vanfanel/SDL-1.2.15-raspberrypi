/*
	SDL - Simple DirectMedia Layer
	Copyright (C) 1997-2012 Sam Lantinga

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

	Sam Lantinga
	slouken@libsdl.org

	Dispmanx driver by Manuel Alfayate Corchete
	redwindwanderer@gmail.com
*/

#include "SDL_config.h"

#include <stdio.h>

#include <string.h>

#include <bcm_host.h>
#include <pthread.h>
#include <stdbool.h>

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../fbcon/SDL_fbmouse_c.h"
#include "../fbcon/SDL_fbevents_c.h"

#define min(a,b) ((a)<(b)?(a):(b))
#define RGB565(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)

/* Dispmanx surface class structures */

struct dispmanx_page
{
	/* Each page contains it's own resource handler 
	 * instead of pointing to in by page number */
	DISPMANX_RESOURCE_HANDLE_T resource;
	bool used;
	/* Each page has it's own mutex for
	 * isolating it's used flag access. */
	pthread_mutex_t page_used_mutex;

	/* This field will allow us to access the 
	 * surface the page belongs to, for the vsync cb. */
	struct dispmanx_surface *surface;	
};

struct dispmanx_surface
{
	/* main surface has 3 pages, menu surface has 1 */
	unsigned int numpages;
	struct dispmanx_page *pages;
	/* The page that's currently on screen for this surface */
	struct dispmanx_page *current_page;

	VC_RECT_T src_rect;
	VC_RECT_T dst_rect;
	VC_RECT_T bmp_rect;

	/* Each surface has it's own element, and the resources are contained one in each page */
	DISPMANX_ELEMENT_HANDLE_T element;
	VC_DISPMANX_ALPHA_T alpha;    
	VC_IMAGE_TYPE_T pixformat;

	/* Internal frame dimensions that we need in the blitting function. */
	int pitch;
};

struct dispmanx_video
{
	uint64_t frame_count;
	DISPMANX_DISPLAY_HANDLE_T display;
	DISPMANX_UPDATE_HANDLE_T update;
	uint32_t vc_image_ptr;

	/* We abstract three "surfaces": main surface, menu surface and black back surface. */
	struct dispmanx_surface *main_surface;
	struct dispmanx_surface *back_surface;

	/* For console blanking */
	int fb_fd;
	uint8_t *fb_addr;
	unsigned int screensize;
	uint8_t *screen_bck;

	/* Total dispmanx video dimensions. Not counting overscan settings. */
	unsigned int dispmanx_width;
	unsigned int dispmanx_height;

	/* For threading */
	pthread_cond_t  vsync_condition;	
	pthread_mutex_t vsync_cond_mutex;
	pthread_mutex_t pending_mutex;
	unsigned int pageflip_pending;

	/* Menu */
	bool menu_active;

	/* SDL expects us to keep track of the video buffer, so we need this. */
	void *pixmem;
} dispvars;

/* Initialization/Query functions */
static int DISPMANX_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **DISPMANX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *DISPMANX_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int DISPMANX_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void DISPMANX_VideoQuit(_THIS);

/* Dispmanx internal surface class functions */
static struct dispmanx_page *DISPMANX_SurfaceGetFreePage(struct dispmanx_surface *surface);
static void DISPMANX_VsyncCB(DISPMANX_UPDATE_HANDLE_T u, void *data);
static void DISPMANX_SurfaceSetup(int width, int height, int visible_pitch, int bpp, int alpha, float aspect, 
	int numpages, int layer, struct dispmanx_surface **surface);
void DISPMANX_SurfaceUpdate(const void *frame, struct dispmanx_surface *surface);
static void Dispmanx_SurfaceFree(struct dispmanx_surface **surface);
static void DISPMANX_BlankConsole();

/* Hardware surface functions */
static void DISPMANX_WaitVBL(_THIS);
static void DISPMANX_WaitIdle(_THIS);
static void DISPMANX_DirectUpdate(_THIS, int numrects, SDL_Rect *rects);

/* We give it memory later, when we init the dispmanx video system, which may not be done if
 * game uses SDL only for input and hence passes 0,0 internal size to SDL_SetVideoMode(). */
struct dispmanx_video *_dispvars = NULL;

static void DISPMANX_InitDispmanx () {
	/* First things first... */	
	_dispvars = calloc(1, sizeof(struct dispmanx_video));
	
	/* Init dispmanx surfaces once. Set internal and
         * external dimensions several times (in SetVideoMode()). */
	bcm_host_init();
   	_dispvars->display = vc_dispmanx_display_open(0 /* LCD */);
	
	/* If the console framebuffer has active overscan settings, 
	 * the user must have overscan_scale=1 in config.txt to have 
	 * the same size for both fb console and dispmanx. */
	graphics_get_display_size(_dispvars->display, &_dispvars->dispmanx_width, &_dispvars->dispmanx_height);

	/* Setup some dispmanx parameters */
	_dispvars->vc_image_ptr     = 0;
	_dispvars->pageflip_pending = 0;	
      
	/* Set surface pointers to NULL so we can know if they're already using resources in
	 * SetVideoMode() */
	_dispvars->main_surface = NULL;
	_dispvars->back_surface = NULL;
 
	/* Initialize the rest of the mutexes and conditions. */
	pthread_cond_init(&_dispvars->vsync_condition, NULL);
	pthread_mutex_init(&_dispvars->vsync_cond_mutex, NULL);
	pthread_mutex_init(&_dispvars->pending_mutex, NULL);
}

/* If no free page is available when called, wait for a page flip. */
static struct dispmanx_page *DISPMANX_SurfaceGetFreePage(struct dispmanx_surface *surface) {
	unsigned i;
	struct dispmanx_page *page = NULL;

	while (!page)
	{
		/* Try to find a free page */
		for (i = 0; i < surface->numpages; ++i) {
			if (!surface->pages[i].used)
			{
				page = (surface->pages) + i;
				break;
			}
		}

		/* If no page is free at the moment,
		 * wait until a free page is freed by vsync CB. */
		if (!page) {
			pthread_mutex_lock(&_dispvars->vsync_cond_mutex);
			pthread_cond_wait(&_dispvars->vsync_condition, &_dispvars->vsync_cond_mutex);
			pthread_mutex_unlock(&_dispvars->vsync_cond_mutex);
		}
	}

	/* We mark the choosen page as used */
	pthread_mutex_lock(&page->page_used_mutex);
	page->used = true;
	pthread_mutex_unlock(&page->page_used_mutex);

	return page;
}

static void DISPMANX_VsyncCB(DISPMANX_UPDATE_HANDLE_T u, void *data)
{
	struct dispmanx_page *page = data;
	struct dispmanx_surface *surface = page->surface;

	/* Marking the page as free must be done before the signaling
	 * so when update_main continues (it won't continue until we signal) 
	 * we can chose this page as free */
	if (surface->current_page) {
		pthread_mutex_lock(&surface->current_page->page_used_mutex);

		/* We mark as free the page that was visible until now */
		surface->current_page->used = false;

		pthread_mutex_unlock(&surface->current_page->page_used_mutex);
	}

	/* The page on which we issued the flip that
	* caused this callback becomes the visible one */
	surface->current_page = page;

	/* These two things must be isolated "atomically" to avoid getting 
	 * a false positive in the pending_mutex test in update_main. */ 
	pthread_mutex_lock(&_dispvars->pending_mutex);

	_dispvars->pageflip_pending--;	
	pthread_cond_signal(&_dispvars->vsync_condition);

	pthread_mutex_unlock(&_dispvars->pending_mutex);
}

static void DISPMANX_SurfaceSetup(int width, 
	int height, 
	int visible_pitch,
	int bpp, 
	int alpha, 
	float aspect, 
	int numpages, 
	int layer,
	struct dispmanx_surface **sp)
{
	int i, dst_width, dst_height, dst_xpos, dst_ypos;
	*sp = calloc(1, sizeof(struct dispmanx_surface));
	struct dispmanx_surface *surface = *sp;
	/* Internal frame dimensions. Pitch is total pitch including info 
	 * between scanlines */
	surface->pitch = visible_pitch;
	surface->numpages = numpages; 
	surface->current_page = NULL; 

	/* Transparency disabled */
	surface->alpha.flags = DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS;
	surface->alpha.opacity = alpha;
	surface->alpha.mask = 0;

	/* The "visible" width obtained from the core pitch. We blit based on 
	 * the "visible" width, for cores with things between scanlines. */
	int visible_width = visible_pitch / (bpp / 8);

	/* Set pixformat depending on bpp */
	switch (bpp){
		case 8:
			surface->pixformat = VC_IMAGE_8BPP;
			break;
		case 16:
			surface->pixformat = VC_IMAGE_RGB565;
			break;
		case 32:
			surface->pixformat = VC_IMAGE_XRGB8888;
			break;
		default:
			return;
	}

	/* Allocate memory for all the pages in each surface
	 * and initialize variables inside each page's struct. */
	surface->pages = calloc(surface->numpages, sizeof(struct dispmanx_page));
	for (i = 0; i < surface->numpages; i++) {
		surface->pages[i].used = false;   
		surface->pages[i].surface = surface;   
		pthread_mutex_init(&surface->pages[i].page_used_mutex, NULL); 
	}

	dst_width  = _dispvars->dispmanx_height * aspect;	
	dst_height = _dispvars->dispmanx_height;

	/* If we obtain a scaled image width that is bigger than the physical screen width,
	* then we keep the physical screen width as our maximun width. */
	if (dst_width > _dispvars->dispmanx_width) 
	   dst_width = _dispvars->dispmanx_width;

	dst_xpos = (_dispvars->dispmanx_width - dst_width) / 2;
	dst_ypos = (_dispvars->dispmanx_height - dst_height) / 2;

	/* We configure the rects now. */
	vc_dispmanx_rect_set(&surface->dst_rect, dst_xpos, dst_ypos, dst_width, dst_height);
	vc_dispmanx_rect_set(&surface->bmp_rect, 0, 0, width, height);	
	vc_dispmanx_rect_set(&surface->src_rect, 0, 0, width << 16, height << 16);	

	for (i = 0; i < surface->numpages; i++) {
		surface->pages[i].resource = vc_dispmanx_resource_create(surface->pixformat, 
			visible_width, height, &(_dispvars->vc_image_ptr));
	}
	/* Add element. */
	_dispvars->update = vc_dispmanx_update_start(0);

	surface->element = vc_dispmanx_element_add(
		_dispvars->update,_dispvars->display, layer, 
		&surface->dst_rect, surface->pages[0].resource, 
		&surface->src_rect, DISPMANX_PROTECTION_NONE,
		&surface->alpha, 0, (DISPMANX_TRANSFORM_T)0);

	vc_dispmanx_update_submit_sync(_dispvars->update);		
}

/* Update a given surface */
void DISPMANX_SurfaceUpdate(const void *frame, struct dispmanx_surface *surface)
{
	struct dispmanx_page *page = NULL;

	/* Wait until last issued flip completes to get a free page. Also, 
	   dispmanx doesn't support issuing more than one pageflip.*/
	pthread_mutex_lock(&_dispvars->pending_mutex);
	if (_dispvars->pageflip_pending > 0)
	{
		pthread_cond_wait(&_dispvars->vsync_condition, &_dispvars->pending_mutex);
	}
	pthread_mutex_unlock(&_dispvars->pending_mutex);

	page = DISPMANX_SurfaceGetFreePage(surface);

	/* Frame blitting */
	vc_dispmanx_resource_write_data(page->resource, surface->pixformat,
		surface->pitch, (void*)frame, &(surface->bmp_rect));

	/* Issue a page flip that will be done at the next vsync. */
	_dispvars->update = vc_dispmanx_update_start(0);

	vc_dispmanx_element_change_source(_dispvars->update, surface->element,
		page->resource);

	vc_dispmanx_update_submit(_dispvars->update, DISPMANX_VsyncCB, (void*)page);

	pthread_mutex_lock(&_dispvars->pending_mutex);
	_dispvars->pageflip_pending++;	
	pthread_mutex_unlock(&_dispvars->pending_mutex);
}

static void DISPMANX_BlankConsole ()
{
	/* Note that a 2-pixels array is needed to accomplish console blanking because with 1-pixel
	 * only the write data function doesn't work well, so when we do the only resource 
	 * change in the surface update function, we will be seeing a distorted console. */
	uint16_t image[2] = {0x0000, 0x0000};
	float aspect = (float)_dispvars->dispmanx_width / (float)_dispvars->dispmanx_height;   

	DISPMANX_SurfaceSetup(2, 2, 4, 16, 255, aspect, 1, -1, &_dispvars->back_surface);
	DISPMANX_SurfaceUpdate(image, _dispvars->back_surface);
}

static void Dispmanx_SurfaceFree(struct dispmanx_surface **sp)
{
	int i;	
	struct dispmanx_surface *surface = *sp; 

	/* What if we run into the vsync cb code after freeing the surface? 
	 * We could be trying to get non-existant lock, signal non-existant condition..
	 * So we wait for any pending flips to complete before freeing any surface. */ 
	pthread_mutex_lock(&_dispvars->pending_mutex);
	if (_dispvars->pageflip_pending > 0)
	{
		pthread_cond_wait(&_dispvars->vsync_condition, &_dispvars->pending_mutex);
	}
	pthread_mutex_unlock(&_dispvars->pending_mutex);

	for (i = 0; i < surface->numpages; i++) { 
		vc_dispmanx_resource_delete(surface->pages[i].resource);
		surface->pages[i].used = false;   
		pthread_mutex_destroy(&surface->pages[i].page_used_mutex); 
	}

	free(surface->pages);

	_dispvars->update = vc_dispmanx_update_start(0);
	vc_dispmanx_element_remove(_dispvars->update, surface->element);
	vc_dispmanx_update_submit_sync(_dispvars->update);		

	free(surface);
	*sp = NULL;
}

static int DISPMANX_Available(void)
{
	return (1);
}

static void DISPMANX_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_VideoDevice *DISPMANX_CreateDevice(int devindex)
{
	SDL_VideoDevice *this;
	
	/* Initialize all variables that we clean on shutdown */
	this = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if ( this ) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateVideoData *)
				SDL_malloc((sizeof *this->hidden));
	}
	if ( (this == NULL) || (this->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( this ) {
			SDL_free(this);
		}
		return(0);
	}
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));
	wait_vbl = DISPMANX_WaitVBL;
	wait_idle = DISPMANX_WaitIdle;
	mouse_fd = -1;
	keyboard_fd = -1;

	/* Set the function pointers */
	this->VideoInit = DISPMANX_VideoInit;
	this->ListModes = DISPMANX_ListModes;
	this->SetVideoMode = DISPMANX_SetVideoMode;
	this->SetColors = DISPMANX_SetColors;
	this->VideoQuit = DISPMANX_VideoQuit;
	this->UpdateRects = NULL;
	this->CheckHWBlit = NULL;
	this->FillHWRect = NULL;
	this->SetHWColorKey = NULL;
	this->SetHWAlpha = NULL;
	this->SetCaption = NULL;
	this->SetIcon = NULL;
	this->IconifyWindow = NULL;
	this->GrabInput = NULL;
	this->GetWMInfo = NULL;
	this->InitOSKeymap = FB_InitOSKeymap;
	this->PumpEvents = FB_PumpEvents;
	this->CreateYUVOverlay = NULL;

	this->free = DISPMANX_DeleteDevice;

	return this;
}

VideoBootStrap DISPMANX_bootstrap = {
	"dispmanx", "Dispmanx Raspberry Pi VC",
	DISPMANX_Available, DISPMANX_CreateDevice
};

static int DISPMANX_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
#if !SDL_THREADS_DISABLED
	/* Create the hardware surface lock mutex */
	hw_lock = SDL_CreateMutex();
	if ( hw_lock == NULL ) {
		SDL_SetError("Unable to create lock mutex");
		DISPMANX_VideoQuit(this);
		return(-1);
	}
#endif

	/* Enable mouse and keyboard support */
	if ( FB_OpenKeyboard(this) < 0 ) {
		DISPMANX_VideoQuit(this);
		return(-1);
	}
	if ( FB_OpenMouse(this) < 0 ) {
		const char *sdl_nomouse;

		sdl_nomouse = SDL_getenv("SDL_NOMOUSE");
		if ( ! sdl_nomouse ) {
			SDL_SetError("Unable to open mouse");
			DISPMANX_VideoQuit(this);
			return(-1);
		}
	}

	vformat->BitsPerPixel = 16;
	vformat->Rmask = 0;
	vformat->Gmask = 0;
	vformat->Bmask = 0;

	/* Just in case, it doesn't hurt. */
	_dispvars = NULL;

	/* We're done! */
	return(0);
}

static SDL_Surface *DISPMANX_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags)
{
	/* Some emulators and games such as PiMame4All initialize dispmanx themselves for GLES
	 * and only use SDL 1.x for input, so this way we know we must skip our initialization 
	 * and modesetting in these cases. */
	if ((width == 0) | (height == 0)) goto go_video_console;

	/* We dp this here and NOT in VideoInit because this way all this can be skipped in case
	 * a game or emu initializes dispmanx itself for EGL/GLES and uses SDL only for input. */
	if (_dispvars == NULL) {
		DISPMANX_InitDispmanx();	
		DISPMANX_BlankConsole();
	}

	bool keep_aspect = !SDL_getenv("SDL_DISPMANX_IGNORE_RATIO");
	float aspect;

	if (keep_aspect) {
		const char *user_aspect = SDL_getenv("SDL_DISPMANX_RATIO");
		if (user_aspect != NULL) {
			aspect = strtof(user_aspect, NULL);
		}
		/* only allow sensible aspect ratios */
		if (aspect < 0.2f || aspect > 6.0f) {
			aspect = (float)width / (float)height;
		}
	} else {
		/* This is unnecesary but allows us to have a general case SurfaceSetup function. */
		aspect = (float)_dispvars->dispmanx_width / (float)_dispvars->dispmanx_height;
	}

	Uint32 Rmask = 0;
	Uint32 Gmask = 0;
	Uint32 Bmask = 0;
	if ( ! SDL_ReallocFormat(current, bpp, Rmask, Gmask, Bmask, 0) ) {
		return(NULL);
	}

	int pitch = width * bpp/8;
	_dispvars->pixmem = calloc( 1, pitch * height);
	current->w = width;
	current->h = height;
	current->pitch  = pitch;
	current->pixels = _dispvars->pixmem;
	/* We need to do this so hardware palette gets adjusted for 8bpp games. */
	current->flags |= SDL_HWPALETTE;

        if ( _dispvars->main_surface != NULL) {
		Dispmanx_SurfaceFree(&_dispvars->main_surface);
	}
		
	DISPMANX_SurfaceSetup(width, height, pitch, bpp, 255, aspect, 3, 0,
	   &_dispvars->main_surface);

	/* IMPORTANT: We can't do this on the Init function or the cursor init code
	 * will try to draw the cursor before the surface is ready! */	
	this->UpdateRects = DISPMANX_DirectUpdate;

	go_video_console:
	if ( FB_EnterGraphicsMode(this) < 0 )
		return(NULL);

	return(current);
}

static void DISPMANX_WaitVBL(_THIS)
{
	return;
}

static void DISPMANX_WaitIdle(_THIS)
{
	return;
}

/* Direct update of the main surface. */
static void DISPMANX_DirectUpdate(_THIS, int numrects, SDL_Rect *rects)
{
	DISPMANX_SurfaceUpdate(_dispvars->pixmem, _dispvars->main_surface);
}

static int DISPMANX_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	int i;
	static unsigned short pal[256];
	struct dispmanx_surface *surface = _dispvars->main_surface;
	//Set up the colormap
	for (i = 0; i < ncolors; i++) {
		pal[i] = RGB565 ((colors[i]).r, (colors[i]).g, (colors[i]).b);
	}
	for (i = 0; i < surface->numpages; i++) {
		vc_dispmanx_resource_set_palette(surface->pages[i].resource, pal, 0, sizeof(pal));
	}
	return(0);
}

static SDL_Rect **DISPMANX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	return((SDL_Rect **)-1);
}

static void DISPMANX_VideoQuit(_THIS)
{
	/* Clear the lock mutex */
	if ( hw_lock ) {
		SDL_DestroyMutex(hw_lock);
		hw_lock = NULL;
	}

	FB_CloseMouse(this);
	FB_CloseKeyboard(this);

	/* If _dispvars is NULL, it means game inits dispmanx itself 
	 * for EGL/GLES or something like that, so we don't have to free anything here.*/
	if (_dispvars != NULL) {	
		/* Free the dispmanx surfaces */
		Dispmanx_SurfaceFree(&_dispvars->main_surface);
		Dispmanx_SurfaceFree(&_dispvars->back_surface);

		/* Destroy surface management mutexes and conditions. */
		pthread_mutex_destroy(&_dispvars->pending_mutex);
		pthread_mutex_destroy(&_dispvars->vsync_cond_mutex);
		pthread_cond_destroy(&_dispvars->vsync_condition);

		/* Close display and deinitialize dispmanx. */
		vc_dispmanx_display_close(_dispvars->display);
		bcm_host_deinit();
		free (_dispvars);
		_dispvars = NULL;
	}
}
