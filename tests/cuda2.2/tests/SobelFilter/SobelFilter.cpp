/*
 * Copyright 1993-2009 NVIDIA Corporation.  All rights reserved.
 *
 * NOTICE TO USER:
 *
 * This source code is subject to NVIDIA ownership rights under U.S. and
 * international Copyright laws.  Users and possessors of this source code
 * are hereby granted a nonexclusive, royalty-free license to use this code
 * in individual and commercial software.
 *
 * NVIDIA MAKES NO REPRESENTATION ABOUT THE SUITABILITY OF THIS SOURCE
 * CODE FOR ANY PURPOSE.  IT IS PROVIDED "AS IS" WITHOUT EXPRESS OR
 * IMPLIED WARRANTY OF ANY KIND.  NVIDIA DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOURCE CODE, INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE.
 * IN NO EVENT SHALL NVIDIA BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL,
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS,  WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION,  ARISING OUT OF OR IN CONNECTION WITH THE USE
 * OR PERFORMANCE OF THIS SOURCE CODE.
 *
 * U.S. Government End Users.   This source code is a "commercial item" as
 * that term is defined at  48 C.F.R. 2.101 (OCT 1995), consisting  of
 * "commercial computer  software"  and "commercial computer software
 * documentation" as such terms are  used in 48 C.F.R. 12.212 (SEPT 1995)
 * and is provided to the U.S. Government only as a commercial end item.
 * Consistent with 48 C.F.R.12.212 and 48 C.F.R. 227.7202-1 through
 * 227.7202-4 (JUNE 1995), all U.S. Government End Users acquire the
 * source code with only those rights set forth herein.
 *
 * Any use of this source code in individual and commercial software must
 * include, in the user documentation and internal comments to the code,
 * the above Disclaimer and U.S. Government End Users Notice.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <GL/glew.h>
#if defined(__APPLE__) || defined(MACOSX)
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#include <cuda_runtime.h>
#include <cutil_inline.h>
#include <cutil_gl_inline.h>
#include <cuda_gl_interop.h>

#include <rendercheck_gl.h>
#include "SobelFilter_kernels.h"

//
// Cuda example code that implements the Sobel edge detection
// filter. This code works for 8-bit monochrome images.
//
// Use the '-' and '=' keys to change the scale factor.
//
// Other keys:
// I: display image
// T: display Sobel edge detection (computed solely with texture)
// S: display Sobel edge detection (computed with texture and shared memory)

void cleanup(void);
void initializeData(char *file) ;

#define MAX_EPSILON_ERROR 5.0f

static char *sSDKsample = "CUDA Sobel Edge-Detection"; 

// Define the files that are to be save and the reference images for validation
const char *sOriginal[] =
{
    "lena_orig.pgm",
    "lena_shared.pgm",
    "lena_shared_tex.pgm",
    NULL
};

const char *sOriginal_ppm[] =
{
    "lena_orig.ppm",
    "lena_shared.ppm",
    "lena_shared_tex.ppm",
    NULL
};

const char *sReference[] =
{
    "ref_orig.pgm",
    "ref_shared.pgm",
    "ref_shared_tex.pgm",
    NULL
};

const char *sReference_ppm[] =
{
    "ref_orig.ppm",
    "ref_shared.ppm",
    "ref_shared_tex.ppm",
    NULL
};

static int wWidth   = 512; // Window width
static int wHeight  = 512; // Window height
static int imWidth  = 0;   // Image width
static int imHeight = 0;   // Image height

// Code to handle Auto verification
const int frameCheckNumber = 4;
int fpsCount = 0;      // FPS count for averaging
int fpsLimit = 8;      // FPS limit for sampling
unsigned int frameCount = 0;
unsigned int g_TotalErrors = 0;
bool       g_Verify = false, g_AutoQuit = false;
unsigned int timer;
unsigned int g_Bpp;
unsigned int g_Index = 0;

bool g_bQAReadback = false;
bool g_bOpenGLQA   = false;
bool g_bFBODisplay = false;

// Display Data
static GLuint pbo_buffer = 0;  // Front and back CA buffers
static GLuint texid = 0;       // Texture for display
unsigned char * pixels = NULL; // Image pixel data on the host    
float imageScale = 1.f;        // Image exposure
enum SobelDisplayMode g_SobelDisplayMode;

// CheckFBO/BackBuffer class objects
CheckRender       *g_CheckRender = NULL;

bool bQuit          = false;

extern "C" void runAutoTest(int argc, char **argv);

#define OFFSET(i) ((char *)NULL + (i))
#define MAX(a,b) ((a > b) ? a : b)

void AutoQATest()
{
    if (g_CheckRender && g_CheckRender->IsQAReadback()) {
        char temp[256];
        g_SobelDisplayMode = (SobelDisplayMode)g_Index;
        sprintf(temp, "%s Cuda Edge Detection (%s)", "AutoTest:", filterMode[g_SobelDisplayMode]);  
	    glutSetWindowTitle(temp);

        g_Index++;
        if (g_Index > 2) {
            g_Index = 0;
            printf("Summary: %d errors!\n", g_TotalErrors);
            printf("Test %s!\n", (g_TotalErrors==0) ? "PASSED" : "FAILED");
            exit(0);
        }
    }
}


void computeFPS()
{
    frameCount++;
    fpsCount++;
    if (fpsCount == fpsLimit-1) {
        g_Verify = true;
    }
    if (fpsCount == fpsLimit) {
        char fps[256];
        float ifps = 1.f / (cutGetAverageTimerValue(timer) / 1000.f);
        sprintf(fps, "%s Cuda Edge Detection (%s): %3.1f fps", 
                ((g_CheckRender && g_CheckRender->IsQAReadback()) ? "AutoTest:" : ""),
				filterMode[g_SobelDisplayMode], ifps);  

        glutSetWindowTitle(fps);
        fpsCount = 0; 
        if (g_CheckRender && !g_CheckRender->IsQAReadback()) fpsLimit = (int)MAX(ifps, 1.f);

        cutilCheckError(cutResetTimer(timer));  
        AutoQATest();
    }
}


// This is the normal display path
void display(void) 
{  
    cutilCheckError(cutStartTimer(timer));  

    // Sobel operation
    Pixel *data = NULL;
    cutilSafeCall(cudaGLMapBufferObject((void**)&data, pbo_buffer));
    sobelFilter(data, imWidth, imHeight, g_SobelDisplayMode, imageScale );
    cutilSafeCall(cudaGLUnmapBufferObject(pbo_buffer));   

    glClear(GL_COLOR_BUFFER_BIT);

    glBindTexture(GL_TEXTURE_2D, texid);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_buffer);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, imWidth, imHeight, 
                   GL_LUMINANCE, GL_UNSIGNED_BYTE, OFFSET(0));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glTexCoord2f(0, 0);
    glVertex2f(0, 1); glTexCoord2f(1, 0);
    glVertex2f(1, 1); glTexCoord2f(1, 1);
    glVertex2f(1, 0); glTexCoord2f(0, 1);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);

    if (g_CheckRender && g_CheckRender->IsQAReadback() && g_Verify) {
        printf("> (Frame %d) readback BackBuffer\n", frameCount);
        g_CheckRender->readback( imWidth, imHeight );
        g_CheckRender->savePPM ( sOriginal_ppm[g_Index], true, NULL );
        if (!g_CheckRender->PPMvsPPM(sOriginal_ppm[g_Index], sReference_ppm[g_Index], MAX_EPSILON_ERROR, 0.15f)) {
            g_TotalErrors++;
        }
        g_Verify = false;
    }
    glutSwapBuffers();

    cutilCheckError(cutStopTimer(timer));  

    computeFPS();

    glutPostRedisplay();
}

void idle(void) {
    glutPostRedisplay();
}

void keyboard( unsigned char key, int /*x*/, int /*y*/) 
{
   char temp[256];

    switch( key) {
        case 27:
            exit (0);
	    break;
	case '-':
	    imageScale -= 0.1f;
        printf("brightness = %4.2f\n", imageScale);
	    break;
	case '=':
	    imageScale += 0.1f;
        printf("brightness = %4.2f\n", imageScale);
	    break;
	case 'i': 
    case 'I':
	    g_SobelDisplayMode = SOBELDISPLAY_IMAGE;
	    sprintf(temp, "Cuda Edge Detection (%s)", filterMode[g_SobelDisplayMode]);  
	    glutSetWindowTitle(temp);
	    break;
	case 's': 
    case 'S':
	    g_SobelDisplayMode = SOBELDISPLAY_SOBELSHARED;
	    sprintf(temp, "Cuda Edge Detection (%s)", filterMode[g_SobelDisplayMode]);  
	    glutSetWindowTitle(temp);
	    break;
	case 't': 
    case 'T':
	    g_SobelDisplayMode = SOBELDISPLAY_SOBELTEX;
	    sprintf(temp, "Cuda Edge Detection (%s)", filterMode[g_SobelDisplayMode]);  
	    glutSetWindowTitle(temp);
        default: break;
    }
    glutPostRedisplay();
}

void reshape(int x, int y) {
    glViewport(0, 0, x, y);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, 0, 1); 
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glutPostRedisplay();
}

void cleanup(void) {
    cutilSafeCall(cudaGLUnregisterBufferObject(pbo_buffer));

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo_buffer);
    glDeleteTextures(1, &texid);
    deleteTexture();

    if (g_CheckRender) {
        delete g_CheckRender; g_CheckRender = NULL;
    }

    cutilCheckError(cutDeleteTimer(timer));  
}

void initializeData(char *file) {
    GLint bsize;
    unsigned int w, h;
    size_t file_length= strlen(file);

    if (!strcmp(&file[file_length-3], "pgm")) {
        if (cutLoadPGMub(file, &pixels, &w, &h) != CUTTrue) {
            printf("Failed to load image file: %s\n", file);
            exit(-1);
        }
        g_Bpp = 1;
    } else if (!strcmp(&file[file_length-3], "ppm")) {
        if (cutLoadPPM4ub(file, &pixels, &w, &h) != CUTTrue) {
            printf("Failed to load image file: %s\n", file);
            exit(-1);
        }
        g_Bpp = 4;
    } else {
        cudaThreadExit();
        exit(-1);
    }
    imWidth = (int)w; imHeight = (int)h;
    setupTexture(imWidth, imHeight, pixels, g_Bpp);

    memset(pixels, 0x0, g_Bpp * sizeof(Pixel) * imWidth * imHeight);

    if (!g_bQAReadback) {
        // use OpenGL Path
        glGenBuffers(1, &pbo_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_buffer); 
        glBufferData(GL_PIXEL_UNPACK_BUFFER, 
                        g_Bpp * sizeof(Pixel) * imWidth * imHeight, 
                        pixels, GL_STREAM_DRAW);  

        glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &bsize); 
        if ((GLuint)bsize != (g_Bpp * sizeof(Pixel) * imWidth * imHeight)) {
            printf("Buffer object (%d) has incorrect size (%d).\n", (unsigned)pbo_buffer, (unsigned)bsize);
            cudaThreadExit();
            exit(-1);
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        cutilSafeCall(cudaGLRegisterBufferObject(pbo_buffer));

        glGenTextures(1, &texid);
        glBindTexture(GL_TEXTURE_2D, texid);
        glTexImage2D(GL_TEXTURE_2D, 0, ((g_Bpp==1) ? GL_LUMINANCE : GL_BGRA), 
                    imWidth, imHeight,  0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
    }
}

void loadDefaultImage( char* loc_exec) {

    printf("Reading image: lena.pgm\n");
    const char* image_filename = "lena.pgm";
    char* image_path = cutFindFilePath(image_filename, loc_exec);
    if (image_path == 0) {
       printf( "Reading image failed.\n");
       exit(EXIT_FAILURE);
    }
    initializeData( image_path );
    cutFree( image_path );
}

void printHelp()
{
    printf("\nUsage: SobelFilter <options>\n");
    printf("\t\t-fbo    (render using FBO display path)\n");
    printf("\t\t-qatest (automatic validation)\n");
    printf("\t\t-file = filename.pgm (image files for input)\n\n");
    bQuit = true;
}

void initGL(int argc, char** argv)
{
    glutInit( &argc, argv);    
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(wWidth, wHeight);
    glutCreateWindow("Cuda Edge Detection");

    glewInit();

    if (g_bFBODisplay) {
        if (!glewIsSupported( "GL_VERSION_2_0 GL_ARB_fragment_program GL_EXT_framebuffer_object" )) {
            fprintf(stderr, "Error: failed to get minimal extensions for demo\n");
            fprintf(stderr, "This sample requires:\n");
            fprintf(stderr, "  OpenGL version 2.0\n");
            fprintf(stderr, "  GL_ARB_fragment_program\n");
            fprintf(stderr, "  GL_EXT_framebuffer_object\n");
            cudaThreadExit();
            exit(-1);
        }
	} else {
		if (!glewIsSupported( "GL_VERSION_1_5 GL_ARB_vertex_buffer_object GL_ARB_pixel_buffer_object" )) {
			fprintf(stderr, "Error: failed to get minimal extensions for demo\n");
			fprintf(stderr, "This sample requires:\n");
			fprintf(stderr, "  OpenGL version 1.5\n");
			fprintf(stderr, "  GL_ARB_vertex_buffer_object\n");
			fprintf(stderr, "  GL_ARB_pixel_buffer_object\n");
            cudaThreadExit();
			exit(-1);
		}
	}

}

void runAutoTest(int argc, char **argv)
{
    printf("[%s] (automated testing w/ readback)\n", sSDKsample);

    if( cutCheckCmdLineFlag(argc, (const char**)argv, "device") ) {
        cutilDeviceInit(argc, argv);
    } else {
        cudaSetDevice( cutGetMaxGflopsDeviceId() );
    }

    if (argc > 1) {
        char *filename;
        if (cutGetCmdLineArgumentstr(argc, (const char **)argv, "file", &filename)) {
            initializeData(filename);
        }
    } else {
        loadDefaultImage( argv[0]);
    }

    g_CheckRender       = new CheckBackBuffer(imWidth, imHeight, sizeof(Pixel), false);
    g_CheckRender->setExecPath(argv[0]);

    Pixel *d_result;
    cutilSafeCall( cudaMalloc( (void **)&d_result, imWidth*imHeight*sizeof(Pixel)) );

    while (g_SobelDisplayMode <= 2) 
    {
        printf("AutoTest: %s <%s>\n", sSDKsample, filterMode[g_SobelDisplayMode]);

        sobelFilter(d_result, imWidth, imHeight, g_SobelDisplayMode, imageScale );

        cutilSafeCall( cudaThreadSynchronize() );

        cudaMemcpy(g_CheckRender->imageData(), d_result, imWidth*imHeight*sizeof(Pixel), cudaMemcpyDeviceToHost);

        g_CheckRender->savePGM(sOriginal[g_Index], false, NULL);

        if (!g_CheckRender->PGMvsPGM(sOriginal[g_Index], sReference[g_Index], MAX_EPSILON_ERROR, 0.15f)) {
            g_TotalErrors++;
        }
        g_Index++;
        g_SobelDisplayMode = (SobelDisplayMode)g_Index;
    }

    cutilSafeCall( cudaFree( d_result ) );
    delete g_CheckRender;

    if (!g_TotalErrors) 
        printf("TEST PASSED!\n");
    else 
        printf("TEST FAILED!\n");
}


int main(int argc, char** argv) 
{
	if (!cutCheckCmdLineFlag(argc, (const char **)argv, "noqatest") ||
	    cutCheckCmdLineFlag(argc, (const char **)argv, "noprompt"))
	{
        g_bQAReadback = true;
        fpsLimit = frameCheckNumber;
    }
    if (argc > 1) {
        if (cutCheckCmdLineFlag(argc, (const char **)argv, "help")) {
            printHelp();
        }
        if (cutCheckCmdLineFlag(argc, (const char **)argv, "glverify"))
		{
            g_bOpenGLQA = true;
            fpsLimit = frameCheckNumber;
        }
        if (cutCheckCmdLineFlag(argc, (const char **)argv, "fbo")) {
            g_bFBODisplay = true;
            fpsLimit = frameCheckNumber;
        }
    }

    if (g_bQAReadback) 
    {
        runAutoTest(argc, argv);
    } else {

        // First initialize OpenGL context, so we can properly set the GL for CUDA.
        // This is necessary in order to achieve optimal performance with OpenGL/CUDA interop.
        initGL( argc, argv );

        // use command-line specified CUDA device, otherwise use device with highest Gflops/s
        if( cutCheckCmdLineFlag(argc, (const char**)argv, "device") ) {
            cutilGLDeviceInit(argc, argv);
        } else {
            cudaGLSetGLDevice (cutGetMaxGflopsDeviceId() );
        }

        int device;
        struct cudaDeviceProp prop;
        cudaGetDevice( &device );
        cudaGetDeviceProperties( &prop, device );
        if( !strncmp( "Tesla", prop.name, 5 ) ){
            printf("This sample needs a card capable of OpenGL and display.\n");
            printf("Please choose a different device with the -device=x argument.\n");
            cudaThreadExit();
            cutilExit(argc, argv);
        }

        cutilCheckError(cutCreateTimer(&timer));
        cutilCheckError(cutResetTimer(timer));  
     
        glutDisplayFunc(display);
        glutKeyboardFunc(keyboard);
        glutReshapeFunc(reshape);
        glutIdleFunc(idle);

        if (g_bOpenGLQA) {
            loadDefaultImage( argv[0] );
        }

        if (argc > 1) {
            char *filename;
            if (cutGetCmdLineArgumentstr(argc, (const char **)argv, "file", &filename)) {
                initializeData(filename);
            }
        } else {
            loadDefaultImage( argv[0]);
        }


        // If code is not printing the USage, then we execute this path.
        if (!bQuit) {
            if (g_bOpenGLQA) {
                g_CheckRender = new CheckBackBuffer(wWidth, wHeight, 4);
                g_CheckRender->setPixelFormat(GL_BGRA);
                g_CheckRender->setExecPath(argv[0]);
                g_CheckRender->EnableQAReadback(true);
            }

            printf("I: display image\n");
            printf("T: display Sobel edge detection (computed with tex)\n");
            printf("S: display Sobel edge detection (computed with tex+shared memory)\n");
            printf("Use the '-' and '=' keys to change the brightness.\n");
            fflush(stdout);
            atexit(cleanup); 
            glutMainLoop();
        }
    }

    cudaThreadExit();
    cutilExit(argc, argv);
}
