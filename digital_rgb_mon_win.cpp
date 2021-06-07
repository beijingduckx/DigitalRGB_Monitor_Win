//
// Digital RGB Display with fx2pipe
// 
// Original by 27-Mar-2021 by Minatsu (@tksm372)
//
// Modified for SDL on Windows and CS2300-CP

#define DW 640
#define DH 200

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <Windows.h>
#include <CyAPI.h>
#include <assert.h>

#define BIT_VSYNC 4
#define BIT_HSYNC 3
#define BIT_R 2
#define BIT_G 1
#define BIT_B 0

// Built-in firmware hex strings.
static const char *firmware[] = {
#include "slave.inc"
    NULL};

#define READ() usb_read()

void finalize(void);

//======================================================================
// USB
//======================================================================
#define VID 0x04b4
#define PID 0x8613
#define IN_EP (6)
#define RX_SIZE (16 * 1024 * 4)
#define XFR_NUM 8
#define READ_SIZE (RX_SIZE * XFR_NUM)
static uint8_t buf[XFR_NUM * RX_SIZE];
static volatile int usb_run_flag = 1;

CCyUSBDevice *USBDevice;
OVERLAPPED ov_ep6[XFR_NUM];
HANDLE ev_ep6[XFR_NUM];

OVERLAPPED ov_ep1;
HANDLE ev_ep1;

CCyBulkEndPoint *ep6;
CCyBulkEndPoint *ep1;
CCyControlEndPoint *cep;

static volatile int ui_run_flag = 1;


//----------------------------------------------------------------------
// USB write RAM
//----------------------------------------------------------------------
#define USB_WRITE_RAM_MAX_SIZE 64
int usb_write_ram(int addr, uint8_t *dat, int size) {

    for (int i = 0; i < size; i += USB_WRITE_RAM_MAX_SIZE) {
        LONG len = (size - i > USB_WRITE_RAM_MAX_SIZE) ? USB_WRITE_RAM_MAX_SIZE : size - i;
        cep->Target = TGT_DEVICE;
        cep->ReqType = REQ_VENDOR;
        cep->Direction = DIR_TO_DEVICE;
        cep->ReqCode = 0xa0;
        cep->Value = addr + i;
        cep->Index = 0;
        bool ret = cep->Write(dat + i, len);
        if (ret == false) {
            fprintf(stderr, "USB: Write Ram at %04x (len %d) failed.\n", addr + i, len);
            return -1;
        }
    }
    return 0;
}

//----------------------------------------------------------------------
// USB load firmware
//----------------------------------------------------------------------
#define FIRMWARE_MAX_SIZE_PER_LINE 64
static uint8_t firmware_dat[FIRMWARE_MAX_SIZE_PER_LINE];
int usb_load_firmware(const char *firmware[]) {
    int ret;

    // Take the CPU into RESET
    uint8_t dat = 1;
    ret = usb_write_ram(0xe600, &dat, sizeof(dat));
    if (ret < 0) {
        return -1;
    }

    // Load firmware
    int size, addr, record_type, tmp_dat;
    for (int i = 0; firmware[i] != NULL; i++) {
        const char *p = firmware[i] + 1;

        // Extract size
        ret = sscanf(p, "%2x", &size);
        assert(ret != 0);
        assert(size <= FIRMWARE_MAX_SIZE_PER_LINE);
        p += 2;

        // Extract addr
        ret = sscanf(p, "%4x", &addr);
        assert(ret != 0);
        p += 4;

        // Extract record type
        ret = sscanf(p, "%2x", &record_type);
        assert(ret != 0);
        p += 2;

        // Write program to EZ-USB's RAM (record_type==0).
        if (record_type == 0) {
            for (int j = 0; j < size; j++) {
                ret = sscanf(p, "%2x", &tmp_dat);
                firmware_dat[j] = tmp_dat & 0xff;
                assert(ret != 0);
                p += 2;
            }

            ret = usb_write_ram(addr, firmware_dat, size);
            if (ret < 0) {
                return -1;
            }
        }
    }

    // Take the CPU out of RESET (run)
    dat = 0;
    ret = usb_write_ram(0xe600, &dat, sizeof(dat));
    if (ret < 0) {
        return -1;
    }

    return 0;
}

HANDLE usb_cond;

static volatile uint64_t usb_received_size = 0;
static volatile int usb_trans_pos = 0;
static int usb_closed_flag = 0;
CRITICAL_SECTION received_size_section;

//----------------------------------------------------------------------
// USB thread for bulk-in transfer
//----------------------------------------------------------------------
static HANDLE h_usb_thread;

struct timeval tv = {0, 1};
DWORD WINAPI usb_run(void *arg) {
    //puts("USB: Start receiving VH-RGB signals.");
    UCHAR *ctx[XFR_NUM];

    // Submit USB transfers
    ep6->SetXferSize(RX_SIZE);

    for (int i = 0; i < XFR_NUM; i++) {
        memset(&ov_ep6[i], 0, sizeof(ov_ep6[0]));
        ev_ep6[i] = ::CreateEvent(NULL, false, false, NULL);
        ov_ep6[i].hEvent = ev_ep6[i];
        ctx[i] = ep6->BeginDataXfer(&buf[i * RX_SIZE], RX_SIZE, &ov_ep6[i]);
    }

    // Waiting transfer completion repeatedly
    int64_t last = timeGetTime();
    int64_t cur;
    int64_t msec;
    float avg = 0;
    int index = 0;
    LONG length = 0;
    bool status;

    while (usb_run_flag) {
        status = ep6->WaitForXfer(&ov_ep6[index], 500);
        if (status == true) {
            length = RX_SIZE;
            ep6->FinishDataXfer(&buf[index * RX_SIZE], length, &ov_ep6[index], ctx[index], NULL);
        }
        ::EnterCriticalSection(&received_size_section);
        usb_received_size += length;
        ::LeaveCriticalSection(&received_size_section);

        if (length != RX_SIZE) {
            //break;   // TODO
        }

        usb_trans_pos = RX_SIZE * index;
        ::SetEvent(usb_cond);

        // Re-submit
        ctx[index] = ep6->BeginDataXfer(&buf[index * RX_SIZE], RX_SIZE, &ov_ep6[index]);
        index++;
        index %= XFR_NUM;
        cur = timeGetTime();
        msec = cur - last;
        if (msec > 1000) {
            ::EnterCriticalSection(&received_size_section);
            int size = usb_received_size;
            usb_received_size = 0;
            ::LeaveCriticalSection(&received_size_section);

            float mbps = size / ((cur - last) / 1000.0) / 1024.0 / 1024.0;
            avg = (!avg) ? mbps : avg * 0.95 + mbps * 0.05;
            //printf("Receiving at %.3f MBps (Avg. %.3f Mbps)\r", mbps, avg);
            last = cur;
        }
    }

    // Close all pending Xfers
    int start_index = index;
    do {
        status = ep6->WaitForXfer(&ov_ep6[index], 100);
        if (status == true) {
            length = RX_SIZE;
            ep6->FinishDataXfer(&buf[index * RX_SIZE], length, &ov_ep6[index], ctx[index], NULL);
            ::CloseHandle(ov_ep6[index].hEvent);
        }
        index = (index + 1) % XFR_NUM;
    } while (index != start_index);

    //puts("USB: Thread finished.");

    return 0;
}

//----------------------------------------------------------------------
// Read one "000VHRGB" signal byte via USB
//----------------------------------------------------------------------
__forceinline static uint8_t usb_read() {
    static unsigned int read_pos = 0;
    static int wait_count = 0;
    DWORD ret;

    while (read_pos == usb_trans_pos) {
        ret = ::WaitForSingleObject(usb_cond, 100);
        ::ResetEvent(usb_cond);
        if (ret != WAIT_OBJECT_0) {
            return 0;
        }
    }
    uint8_t dat = buf[read_pos++];
    read_pos %= READ_SIZE;
    return dat;
}

void send_command(uint8_t *data, LONG length) {
    // Stop usb thread
    usb_run_flag = 0;
    ::WaitForSingleObject(h_usb_thread, INFINITE);

    ep1->XferData(data, length, NULL);

    // Restart usb thread
    usb_run_flag = 1;
    h_usb_thread = ::CreateThread(NULL, NULL, usb_run, NULL, NULL, NULL);
}

static unsigned short h_pixels;

void set_pll(void) 
{
    uint32_t ratio_val;
    uint8_t ep1_data[5];

    // Send command
    ratio_val = (h_pixels * 2) << 4;
    ep1_data[0] = 0x01;    // Set PLL
    ep1_data[1] = ratio_val >> 16;
    ep1_data[2] = ratio_val >> 8;
    ep1_data[3] = ratio_val & 0xff;
    ep1_data[4] = 0x00;

    send_command(ep1_data, sizeof(ep1_data));
}

void restart_usb(void) {
    uint8_t ep1_data[2];

    ep1_data[0] = 0x02;  // Reset EP6
    ep1_data[1] = 0x00;  // no meanings

    send_command(ep1_data, sizeof(ep1_data));
}


DWORD WINAPI draw_run(void *arg) {
    unsigned int v_porch;
    unsigned int h_porch;

    // The window we'll be rendering to
    SDL_Window *window = NULL;

    // The surface contained by the window
    SDL_Surface *screenSurface = NULL;
    SDL_Renderer *Renderer = NULL;
    SDL_Texture *Texture = NULL;
    SDL_Palette *Palette = NULL;

    auto set_title = [&]() {
        char tmp[100];
        snprintf(tmp, sizeof(tmp), "Digital RGB Display : H_TOTAL=%d", h_pixels);
        SDL_SetWindowTitle(window, tmp); 
    };

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        ::MessageBoxA(NULL, "SDL could not initialize", NULL, MB_OK);
        return -1;
    }
    // Create window
    window = SDL_CreateWindow("Digital RGB Display", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, DW, DH * 2, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        ::MessageBoxA(NULL, "Window could not be created", NULL, MB_OK);
        return -1;
    }

    Renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (Renderer == NULL) {
        ::MessageBoxA(NULL, "Coould not create renderer", NULL, MB_OK);
        return -1;
    }
    SDL_SetRenderDrawColor(Renderer, 0x00, 0x00, 0x00, 0xff);
    SDL_RenderClear(Renderer);
    Palette = SDL_AllocPalette(8);
    screenSurface = SDL_CreateRGBSurface(0, DW, DH, 8, 0, 0, 0, 0);

    uint8_t d = 0;
    uint8_t vmask = 1 << BIT_VSYNC;
    uint8_t hmask = 1 << BIT_HSYNC;
    uint8_t vhmask = vmask | hmask;
    LONG len;
    SDL_Color aColor;

    aColor.a = 0xff;
    aColor.r = 0x00;
    aColor.g = 0x00;
    aColor.b = 0x00;
    SDL_SetPaletteColors(Palette, &aColor, 0, 1);
    aColor.r = 0x00;
    aColor.g = 0x00;
    aColor.b = 0xff;
    SDL_SetPaletteColors(Palette, &aColor, 1, 1);
    aColor.r = 0x00;
    aColor.g = 0xff;
    aColor.b = 0x00;
    SDL_SetPaletteColors(Palette, &aColor, 2, 1);
    aColor.r = 0x00;
    aColor.g = 0xff;
    aColor.b = 0xff;
    SDL_SetPaletteColors(Palette, &aColor, 3, 1);
    aColor.r = 0xff;
    aColor.g = 0x00;
    aColor.b = 0x00;
    SDL_SetPaletteColors(Palette, &aColor, 4, 1);
    aColor.r = 0xff;
    aColor.g = 0x00;
    aColor.b = 0xff;
    SDL_SetPaletteColors(Palette, &aColor, 5, 1);
    aColor.r = 0xff;
    aColor.g = 0xff;
    aColor.b = 0x00;
    SDL_SetPaletteColors(Palette, &aColor, 6, 1);
    aColor.r = 0xff;
    aColor.g = 0xff;
    aColor.b = 0xff;
    SDL_SetPaletteColors(Palette, &aColor, 7, 1);

    SDL_SetSurfacePalette(screenSurface, Palette);
    SDL_Event e;

    v_porch = 36;
    h_porch = 128;

    h_pixels = 896;  // 896.. X1/turbo,  912 for Pasopia7;

    memset(&ov_ep1, 0, sizeof(ov_ep1));
    ev_ep1 = ::CreateEvent(NULL, false, false, NULL);
    ov_ep1.hEvent = ev_ep1;

    while (usb_run_flag) {
        // Wait V-Sync
        while (READ() & vmask)
            ; // wait untill low
        while (!(READ() & vmask))
            ; // wait untill hi

        // Skip V-Sync back porch
        for (int i = 0; i < v_porch; i++) {
            while (READ() & hmask)
                ; // wait untill low
            while (!(READ() & hmask))
                ; // wait untill hi
        }

        int x, y;

        uint8_t *vram = (uint8_t *)screenSurface->pixels;
        uint8_t *p = vram;

        for (y = 0; y < DH; y++) {
            // Wait H-Sync
            while (READ() & hmask)
                ; // wait untill low
            while (!(READ() & hmask))
                ; // wait untill hi

            // Skip H-Sync back porch
            for (x = 0; x < h_porch - 1; x++) {
#ifdef USE_CP2300
                READ();
#endif
                READ();
            }
            p = &vram[y * DW];
            for (x = 0; x < DW; x++) {
#ifdef USE_CP2300
                READ();
#endif
                d = READ();
                if ((~d) & vhmask) {
                    y = DH; // Sync is lost, skip this frame
                    break;
                }
                *p++ = d & 7;
            }
        }

        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                usb_run_flag = 0;
            } else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_UP:
                    v_porch++;
                    break;
                case SDLK_DOWN:
                    v_porch--;
                    break;

                case SDLK_LEFT:
                    h_porch++;
                    break;
                case SDLK_RIGHT:
                    h_porch--;
                    break;

#ifdef USE_CP2300
                case SDLK_a:
                    h_pixels++;
                    set_pll();					
                    set_title();
                    break;

                case SDLK_s:
                    h_pixels--;
                    set_pll();
                    set_title();
                    break;
#endif

                case SDLK_x:
                    restart_usb();
                    break;

                default:
                    break;
                }
            }
        }
        Texture = SDL_CreateTextureFromSurface(Renderer, screenSurface);
        SDL_RenderCopy(Renderer, Texture, NULL, NULL);
        SDL_DestroyTexture(Texture);
        SDL_RenderPresent(Renderer);
    }

    // Žg‚¢I‚í‚Á‚½‚ç‰ð•ú
    SDL_DestroyWindow(window);
    SDL_Quit();

    ::CloseHandle(ov_ep1.hEvent);
}

//======================================================================
// Main
//======================================================================
int main(int argc, char *argv[]) {
    setvbuf(stdout, (char *)NULL, _IONBF, 0);
    int ret;
    USBDevice = new CCyUSBDevice(NULL);

    // Initialize USB
    int devices = USBDevice->DeviceCount();
    int index = 0;

    do {
        USBDevice->Open(index);
        if (USBDevice->VendorID == VID && USBDevice->ProductID == PID)
            break;
        index++;
    } while (index < devices);

    if (devices == 0 || index == devices) {
        ::MessageBoxA(NULL, "EZ-USB is not connected.", "Digital RGB Display", MB_OK);
        return -1;
    }

    // Search endpoints
    USBDevice->SetAltIntfc(1);

    cep = USBDevice->ControlEndPt;
    assert(cep != NULL);

    for (index = 0; index < USBDevice->EndPointCount(); index++) {
        CCyUSBEndPoint *p = USBDevice->EndPoints[index];

        if (p->Address == 0x01) { // Bulk OUT
            ep1 = dynamic_cast<CCyBulkEndPoint *>(p);
        } else if (p->Address == 0x86) { // Bulk IN
            ep6 = dynamic_cast<CCyBulkEndPoint *>(p);
        }
    }

    // load firmware
    //printf("Main: Firmware download...");
    if (usb_load_firmware(firmware) >= 0) {
        //puts("finished.");
    } else {
        ::MessageBoxA(NULL, "Firmware downloading failed.", "Digital RGB Display", MB_OK);
        return -1;
    }

    ::InitializeCriticalSection(&received_size_section);
    usb_cond = ::CreateEventA(NULL, TRUE, FALSE, NULL);
    ::ResetEvent(usb_cond);

    h_usb_thread = ::CreateThread(NULL, 0, usb_run, NULL, 0, NULL);

    if (h_usb_thread == INVALID_HANDLE_VALUE) {
        MessageBoxA(NULL, "Main: Failed to start USB thread", "Digital RGB Display", MB_OK);
        return -1;
    }

    draw_run(NULL);

    finalize();

    delete USBDevice;
}

void finalize() {
    //puts("\nMain: Finalizing...");

    usb_closed_flag = 1;

    if(::WaitForSingleObject(h_usb_thread, 1000) != WAIT_OBJECT_0){
           //perror("Main: Failed to join USB thread.");
    } else {
        //puts("Main: USB thread joined.");
    }
    //puts("Main: USB device closed.");

    CloseHandle(h_usb_thread);
    CloseHandle(usb_cond);
}
