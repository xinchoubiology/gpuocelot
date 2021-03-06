/*
 * Copyright 1993-2010 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
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
#include "FunctionPointers_kernels.h"

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
void initializeData(char *file, int argc, char **argv) ;

#define MAX_EPSILON_ERROR 5.0f

static char *sSDKsample = "CUDA Function Pointers (SobelFilter)"; 

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
static int blockOp = 0;
static int pointOp = 1;

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
struct cudaGraphicsResource *cuda_pbo_resource; // CUDA Graphics Resource (to transfer PBO)

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
            printf("%s\n", (g_TotalErrors==0) ? "PASSED" : "FAILED");
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

    // map PBO to get CUDA device pointer
	cutilSafeCall(cudaGraphicsMapResources(1, &cuda_pbo_resource, 0));
    size_t num_bytes; 
    cutilSafeCall(cudaGraphicsResourceGetMappedPointer((void **)&data, &num_bytes,  
						       cuda_pbo_resource));
    //printf("CUDA mapped PBO: May access %ld bytes\n", num_bytes);
	
	sobelFilter(data, imWidth, imHeight, g_SobelDisplayMode, imageScale, blockOp, pointOp );
    cutilSafeCall(cudaGraphicsUnmapResources(1, &cuda_pbo_resource, 0));

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
		break;
	case 'b':
	case 'B':
		blockOp = (blockOp + 1)%LAST_BLOCK_FILTER;
		break;
	case 'p':
	case 'P':
		pointOp = (pointOp +1)%LAST_POINT_FILTER;
		break;
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
	cutilSafeCall(cudaGraphicsUnregisterResource(cuda_pbo_resource));

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo_buffer);
    glDeleteTextures(1, &texid);
    deleteTexture();

    if (g_CheckRender) {
        delete g_CheckRender; g_CheckRender = NULL;
    }

    cutilCheckError(cutDeleteTimer(timer));  
}

void initializeData(char *file, int argc, char **argv) {
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
		cutilExit(argc, argv);
    }
    imWidth = (int)w; imHeight = (int)h;
    setupTexture(imWidth, imHeight, pixels, g_Bpp);
	// copy function pointer tables to host side for later use
	setupFunctionTables();

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
			cutilExit(argc, argv);
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

		 // register this buffer object with CUDA
	    cutilSafeCall(cudaGraphicsGLRegisterBuffer(&cuda_pbo_resource, pbo_buffer, cudaGraphicsMapFlagsWriteDiscard));	

        glGenTextures(1, &texid);
        glBindTexture(GL_TEXTURE_2D, texid);
        glTexImage2D(GL_TEXTURE_2D, 0, ((g_Bpp==1) ? GL_LUMINANCE : GL_BGRA), 
                    imWidth, imHeight,  0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
    }
}

void loadDefaultImage( int argc, char ** argv ) {

    printf("Reading image: lena.pgm\n");
    const char* image_filename = "lena.pgm";
    char* image_path = cutFindFilePath(image_filename, argv[0] );
    if (image_path == 0) {
       printf( "Reading image failed.\n");
       exit(EXIT_FAILURE);
    }
    initializeData( image_path, argc, argv );
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

void initGL(int *argc, char** argv)
{
    glutInit( argc, argv );    
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
            cutilExit(*argc, argv);
        } 
    } else {
        if (!glewIsSupported( "GL_VERSION_1_5 GL_ARB_vertex_buffer_object GL_ARB_pixel_buffer_object" )) {
            fprintf(stderr, "Error: failed to get minimal extensions for demo\n");
            fprintf(stderr, "This sample requires:\n");
            fprintf(stderr, "  OpenGL version 1.5\n");
            fprintf(stderr, "  GL_ARB_vertex_buffer_object\n");
            fprintf(stderr, "  GL_ARB_pixel_buffer_object\n");
            cudaThreadExit();
            cutilExit(*argc, argv);
        }
    }
}

bool checkCUDAProfile(int dev)
{
    int runtimeVersion = 0;     

    cudaDeviceProp deviceProp;
    cudaGetDeviceProperties(&deviceProp, dev);

    fprintf(stderr,"\nDevice %d: \"%s\"\n", dev, deviceProp.name);
    cudaRuntimeGetVersion(&runtimeVersion);
    fprintf(stderr,"  CUDA Runtime Version:\t%d.%d\n", runtimeVersion/1000, (runtimeVersion%100)/10);
    fprintf(stderr,"  CUDA SM Capability  :\t%d.%d\n", deviceProp.major, deviceProp.minor);

    if( runtimeVersion/1000 >= 3 && runtimeVersion%100 >= 1 && deviceProp.major >= 2 ) {
        return true;
    } else {
        return false;
    }
}

int findCapableDevice(int argc, char **argv)
{
    int dev;
    int bestDev = -1;

    int deviceCount = 0;
    if (cudaGetDeviceCount(&deviceCount) != cudaSuccess) {
        fprintf(stderr, "cudaGetDeviceCount FAILED CUDA Driver and Runtime version may be mismatched.\n");
        fprintf(stderr, "\nFAILED\n");
        cudaThreadExit();
        cutilExit(argc, argv);
    }
    for (dev = 0; dev < deviceCount; ++dev) {
        cudaDeviceProp deviceProp;
        cudaGetDeviceProperties(&deviceProp, dev);

        if (dev == 0) {
            // This function call returns 9999 for both major & minor fields, if no CUDA capable devices are present
            if (deviceProp.major == 9999 && deviceProp.minor == 9999)
                fprintf(stderr,"There is no device supporting CUDA.\n");
            else if (deviceCount == 1)
                fprintf(stderr,"There is 1 device supporting CUDA\n");
            else
                fprintf(stderr,"There are %d devices supporting CUDA\n", deviceCount);
        }

        if( checkCUDAProfile( dev ) ) {
            fprintf(stderr,"\nFound capable device: %d\n", dev );
            if( bestDev == -1 ) { 
                bestDev = dev;
                fprintf(stderr, "Setting active device to %d\n", bestDev );
            }
        }
    }

    if( bestDev == -1 ) {
        fprintf(stderr, "\nNo configuration with available capabilities was found.  Test has been waived.\n");
        fprintf(stderr, "This sample requires:\n");
        fprintf(stderr, "\tGPU Device Compute   >= 2.0 is required\n");
        fprintf(stderr, "\tCUDA Runtime Version >= 3.1 is required\n");
        fprintf(stderr, "PASSED\n");
    }
    return bestDev;
}

void runAutoTest(int argc, char **argv)
{
    printf("[%s] (automated testing w/ readback)\n", sSDKsample);

	if( cutCheckCmdLineFlag(argc, (const char**)argv, "device") ) {
		cutilDeviceInit(argc, argv);
		int device;
		cudaGetDevice( &device );
		if( checkCUDAProfile( device ) == false ) {
			cudaThreadExit();
		    cutilExit(argc, argv);
		}
	} else {
		int dev = findCapableDevice(argc, argv);
		if( dev != -1 ) 
			cudaSetDevice( dev );
		else {
			cudaThreadExit();
		    cutilExit(argc, argv);
		}
	}

    loadDefaultImage( argc, argv );

    if (argc > 1) {
        char *filename;
        if (cutGetCmdLineArgumentstr(argc, (const char **)argv, "file", &filename)) {
            initializeData(filename, argc, argv);
        }
    } else {
        loadDefaultImage( argc, argv );
    }

    g_CheckRender       = new CheckBackBuffer(imWidth, imHeight, sizeof(Pixel), false);
    g_CheckRender->setExecPath(argv[0]);

    Pixel *d_result;
    cutilSafeCall( cudaMalloc( (void **)&d_result, imWidth*imHeight*sizeof(Pixel)) );

    while (g_SobelDisplayMode <= 2) 
    {
        printf("AutoTest: %s <%s>\n", sSDKsample, filterMode[g_SobelDisplayMode]);

        sobelFilter(d_result, imWidth, imHeight, g_SobelDisplayMode, imageScale, blockOp, pointOp );

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
        printf("PASSED\n");
    else 
        printf("FAILED\n");
}


int main(int argc, char** argv) 
{
    printf("[%s]\n", sSDKsample);
    if (argc > 1) {
        if (cutCheckCmdLineFlag(argc, (const char **)argv, "help")) {
            printHelp();
        }
		if (cutCheckCmdLineFlag(argc, (const char **)argv, "qatest") ||
		    cutCheckCmdLineFlag(argc, (const char **)argv, "noprompt"))
		{
            g_bQAReadback = true;
            fpsLimit = frameCheckNumber;
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
    } 
    else 
    {
        // First initialize OpenGL context, so we can properly set the GL for CUDA.
        // This is necessary in order to achieve optimal performance with OpenGL/CUDA interop.
        initGL( &argc, argv );

        // use command-line specified CUDA device if possible, otherwise search for capable device
        if( cutCheckCmdLineFlag(argc, (const char**)argv, "device") ) {
            cutilGLDeviceInit(argc, argv);
			int device;
			cudaGetDevice( &device );
			if( checkCUDAProfile( device ) == false ) {
				cudaThreadExit();
				cutilExit(argc, argv);
			}
        } else {
            //cudaGLSetGLDevice (cutGetMaxGflopsDeviceId() );
			int dev = findCapableDevice(argc, argv);
			if( dev != -1 ) 
				cudaGLSetGLDevice( dev );
			else {
				cudaThreadExit();
				cutilExit(argc, argv);
			}
        }

        cutilCheckError(cutCreateTimer(&timer));
        cutilCheckError(cutResetTimer(timer));  
     
        glutDisplayFunc(display);
        glutKeyboardFunc(keyboard);
        glutReshapeFunc(reshape);
        glutIdleFunc(idle);

        if (g_bOpenGLQA) {
            loadDefaultImage( argc, argv );
        }

        if (argc > 1) {
            char *filename;
            if (cutGetCmdLineArgumentstr(argc, (const char **)argv, "file", &filename)) {
                initializeData(filename, argc, argv);
            }
        } else {
            loadDefaultImage( argc, argv );
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
			printf("b: switch block filter operation (mean/Sobel)\n");
			printf("p: swtich point filter operation (threshold on/off)\n");
            fflush(stdout);
            atexit(cleanup); 
            glutMainLoop();
        }
    }

    cudaThreadExit();
    cutilExit(argc, argv);
}
