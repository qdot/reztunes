/*
 *  rezTunes.c
 *  rezTunes 1.0
 *
 *  Created by Bryn Davies on 25/05/07.
 *  curious.jp@gmail.com / curious@progsoc.org
 *  http://progsoc.org/~curious/
 *
 *  Copyright 2007 Bryn Davies. All rights reserved.
 *  ( But then relicensed - see README )
 *  
 *  Ported to libtrancevibe and windows by Kyle Machulis (kyle@nonpolynomial.com)
 *  http://www.nonpolynomial.com
 */

#if TARGET_OS_WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "iTunesVisualAPI.h"
#include "trancevibe.h"

#if TARGET_OS_WIN32
#define	MAIN iTunesPluginMain
#define IMPEXP	__declspec(dllexport)
#else
#define IMPEXP
#define	MAIN main
#endif

#define kTVisualPluginName	"\010rezTunes"
#define	kTVisualPluginCreator	'hook'

#define kTVisualPluginMajorVersion 1
#define kTVisualPluginMinorVersion 2
#define	kTVisualPluginReleaseStage	finalStage
#define	kTVisualPluginNonFinalRelease 0

/*
 * USB identifier keys for the Rez Trance Vibrator
 * from ASCII Corporation.
 */

#define PRODUCTID 0x064f
#define VENDORID 0x0b49

/*
 * Parameters of the beat detection code.
 *   RETAINMS - Length of the audio "memory" in milliseconds.
 *   RETAINSAMPLES - How many samples should be taken during this time.
 *
 *   SENSITIVITY - To make "beat", a signal must be this many times over 
 *     the retained average in it's subband.
 *   MINPEAK - It must also be MINPEAK greater than the local retained
 *     average.
 *
 *  FREQUENCYBANDS - The spectrum is divided up into this many channels.
 *
 *  DECAY - The speed at which the motor winds down.
 *
 *  FALLOFF - Beats in higher bands will produce slower vibrations, how
 *    much slower depends on this variable.
 */

#define RETAINSAMPLES 20
#define RETAINMS 500
#define SENSITIVITY 1.8
#define MINPEAK 1.5
#define FREQUENCYBANDS 9
#define DECAY 10
#define FALLOFF 90

struct list_element {
	float value;
	struct list_element* next;
};
typedef struct list_element list_element;

struct VisualPluginData {
	void				*appCookie;
	ITAppProcPtr		appProc;
	ITFileSpec			pluginFileSpec;

#if TARGET_OS_MAC
	CGrafPtr			destPort;
#else
	HWND destPort;
#endif

	Rect				destRect;

	OptionBits			destOptions;
	UInt32				destBitDepth;
	RenderVisualData	renderData;
	UInt32				renderTimeStampID;
	ITTrackInfo			trackInfo;
	ITStreamInfo		streamInfo;
	Boolean				playing;
	Boolean				padding[ 3 ];

	/*
	 * Things will break if the struct is not
	 * at least this big.  I'm not sure as to why.
	 */
	Boolean				running;
	Boolean				hasVibe;
	UInt8				motorSpeed;
	SInt32				volume;
	list_element*       energyHistory[ FREQUENCYBANDS ];
	int                 energyCount[ FREQUENCYBANDS ];
	float               energyAggregate[ FREQUENCYBANDS ];
	trancevibe          tv;
};
typedef struct VisualPluginData VisualPluginData;


/*
 * Function Prototypes.
 */
extern OSStatus iTunesPluginMainMachO( OSType message, PluginMessageInfo *messageInfo, void *refCon );
static OSStatus VisualPluginHandler( OSType message, VisualPluginMessageInfo *messageInfo, void *refCon );
static OSStatus RegisterVisualPlugin( PluginMessageInfo *messageInfo );

static void MemClear( LogicalAddress dest, SInt32 length );

static void ProcessRenderData( VisualPluginData *vPD, const RenderVisualData *renderData );
static void UpdateScreen( VisualPluginData *vPD );
static OSStatus ChangeVisualPort(VisualPluginData *visualPluginData,GRAPHICS_DEVICE destPort,const Rect *destRect);

static void SetupDevice( VisualPluginData *vPD );
static void SetSpeed( VisualPluginData *vPD );
static void CleanupDevice( VisualPluginData *vPD );


// ChangeVisualPort
//
static OSStatus ChangeVisualPort(VisualPluginData *visualPluginData,GRAPHICS_DEVICE destPort,const Rect *destRect)
{
	OSStatus		status;
	
	status = noErr;
			
	visualPluginData->destPort = destPort;
	if (destRect != nil)
		visualPluginData->destRect = *destRect;

	return status;
}

/*
 * Central messaging and dispatch function.
 */
static OSStatus VisualPluginHandler( OSType message, VisualPluginMessageInfo *messageInfo, void *refCon )
{
	VisualPluginData *vPD;
	OSStatus status;
	UInt8 oldSpeed = 0;
	static int has_init = 0;

	vPD = ( VisualPluginData * ) refCon;
	if (has_init) oldSpeed = vPD->motorSpeed;
	
	status = noErr;
	
	switch( message )
	{
		/*
		 * Allocate a new structure, and start chasing down those vibrators.
		 */
		case kVisualPluginInitMessage:
		{
			int i = 0;
			has_init = 1;
			vPD = ( VisualPluginData * ) malloc(sizeof( VisualPluginData ) );
			if( vPD == nil )
			{
				status = memFullErr;
				break;
			}

			vPD->appCookie	= messageInfo->u.initMessage.appCookie;
			vPD->appProc	= messageInfo->u.initMessage.appProc;
			vPD->motorSpeed = 0;
			vPD->running = false;
			vPD->hasVibe = false;

			for(i = 0; i < FREQUENCYBANDS; ++i)
			{
				vPD->energyHistory[i] = (list_element*) malloc(sizeof(list_element));
				vPD->energyHistory[i]->next = nil;
				vPD->energyCount[i] = 0;
				vPD->energyAggregate[i] = 0;
			}
			
			vPD->tv = nil;
			SetupDevice(vPD);
			messageInfo->u.initMessage.refCon = (void*) vPD;
			break;
		}
		/*
		 * Cleanup.
		 */
		case kVisualPluginCleanupMessage:
			{
				int i;
				CleanupDevice( vPD );
				for(i = 0; i < FREQUENCYBANDS; ++i)
				{	
					list_element* cleanup = vPD->energyHistory[i];
					list_element* old_element;
					while(cleanup != nil)
					{
						old_element = cleanup;
						cleanup = cleanup->next;
						free(old_element);
					}
				}
				if( vPD != nil) free( vPD );
				break;
			}

		case kVisualPluginShowWindowMessage:
			vPD->destOptions = messageInfo->u.showWindowMessage.options;
			status = ChangeVisualPort( vPD,
#if TARGET_OS_WIN32
										messageInfo->u.setWindowMessage.window,
#else
										messageInfo->u.setWindowMessage.port,
#endif
										&messageInfo->u.showWindowMessage.drawRect);			
			vPD->destRect = messageInfo->u.showWindowMessage.drawRect;
			vPD->running = true;
			if(status == noErr)
			{
				UpdateScreen( vPD );
			}
			break;
		
		case kVisualPluginHideWindowMessage:
			vPD->running = false;
			break;
		
		case kVisualPluginUpdateMessage:
			UpdateScreen( vPD );
			break;
		
		case kVisualPluginRenderMessage:
			vPD->renderTimeStampID	= messageInfo->u.renderMessage.timeStampID;
			ProcessRenderData( vPD, messageInfo->u.renderMessage.renderData );
			UpdateScreen( vPD );
			break;
		
		case kVisualPluginPlayMessage:
			vPD->volume = messageInfo->u.playMessage.volume;
		case kVisualPluginUnpauseMessage:
			vPD->playing = true;
			break;

		case kVisualPluginStopMessage:
		case kVisualPluginPauseMessage:
			vPD->playing = false;
			break;

		case kVisualPluginIdleMessage:
			break;

		
		case kVisualPluginEnableMessage:
		case kVisualPluginDisableMessage:
			return noErr;

		default:
			status = unimpErr;
			break;
	}
	
	if( has_init == 1)
	{
		if (vPD->playing == false || vPD->running == false) vPD->motorSpeed = 0;	
		if (oldSpeed != vPD->motorSpeed && vPD->hasVibe == true ) SetSpeed( vPD );
	}
	
	return noErr;	
}

static OSStatus RegisterVisualPlugin( PluginMessageInfo *messageInfo )
{
	OSStatus			status;
	PlayerMessageInfo	playerMessageInfo;
	Str255				pluginName = kTVisualPluginName;

	MemClear( &playerMessageInfo.u.registerVisualPluginMessage, sizeof( playerMessageInfo.u.registerVisualPluginMessage ) );
	memcpy(&playerMessageInfo.u.registerVisualPluginMessage.name[0], &pluginName[0], pluginName[0]+1);
	SetNumVersion( &playerMessageInfo.u.registerVisualPluginMessage.pluginVersion, kTVisualPluginMajorVersion, kTVisualPluginMinorVersion, kTVisualPluginReleaseStage, kTVisualPluginNonFinalRelease );

	#if TARGET_OS_MAC					
		CFStringRef tCFStringRef = CFStringCreateWithPascalString( kCFAllocatorDefault, pluginName, kCFStringEncodingUTF8 );
		if ( tCFStringRef ) 
		{
			CFIndex length = CFStringGetLength( tCFStringRef );
			if ( length > 255 ) 
			{
				length = 255;
			}
			playerMessageInfo.u.registerVisualPluginMessage.unicodeName[0] = CFStringGetBytes( tCFStringRef, CFRangeMake( 0, length ), kCFStringEncodingUnicode, 0, FALSE, (UInt8 *) &playerMessageInfo.u.registerVisualPluginMessage.unicodeName[1], 255, NULL );
			CFRelease( tCFStringRef );
		}
	#endif TARGET_OS_MAC

#if TARGET_OS_MAC
	playerMessageInfo.u.registerVisualPluginMessage.options					= kVisualProvidesUnicodeName;
#else
	playerMessageInfo.u.registerVisualPluginMessage.options					= 0;
#endif
	playerMessageInfo.u.registerVisualPluginMessage.handler					= (VisualPluginProcPtr)VisualPluginHandler;
	playerMessageInfo.u.registerVisualPluginMessage.registerRefCon			= 0;
	playerMessageInfo.u.registerVisualPluginMessage.creator					= kTVisualPluginCreator;
	playerMessageInfo.u.registerVisualPluginMessage.timeBetweenDataInMS		= RETAINMS / RETAINSAMPLES;
	playerMessageInfo.u.registerVisualPluginMessage.numWaveformChannels		= 0;
	playerMessageInfo.u.registerVisualPluginMessage.numSpectrumChannels		= 2;
	playerMessageInfo.u.registerVisualPluginMessage.minWidth				= 64;
	playerMessageInfo.u.registerVisualPluginMessage.minHeight				= 64;
	playerMessageInfo.u.registerVisualPluginMessage.maxWidth				= 32767;
	playerMessageInfo.u.registerVisualPluginMessage.maxHeight				= 32767;
	playerMessageInfo.u.registerVisualPluginMessage.minFullScreenBitDepth	= 0;
	playerMessageInfo.u.registerVisualPluginMessage.maxFullScreenBitDepth	= 0;
	playerMessageInfo.u.registerVisualPluginMessage.windowAlignmentInBytes	= 0;
	
	status = PlayerRegisterVisualPlugin( messageInfo->u.initMessage.appCookie, messageInfo->u.initMessage.appProc, &playerMessageInfo );
		
	return status;
}

static void MemClear( LogicalAddress dest, SInt32 length )
{
	register unsigned char	*ptr = ( unsigned char * ) dest;
	while( length-- > 0 ) *ptr++ = 0;
}

/*
 * This function should be called every RETAINMS / RETAINSAMPLES milliseconds
 * with a new dump of processed spectrum data.  The spectrum is traversed in
 * bands, and an average sonic energy is determined for the band.
 *
 * This is compared with RETAINSAMPLES historical records to detect if the
 * criteria for a "beat" has been found.  If the beat, considered as a multiple
 * of the channels historical average is stronger than any previously recorded
 * beat, it becomes the current dominant beat.
 *
 * Dominant beats affect the motor speed in relation to which band they
 * were discovered in.  If no beat is found, the motor speed decays.
 */

static void ProcessRenderData( VisualPluginData *vPD, const RenderVisualData *renderData )
{
	int	bandindex, start, end;
	float bestratio = 0;
	
	if( renderData == nil ) return;
	
	start = end = 0;
			
	for( bandindex = 0; bandindex < FREQUENCYBANDS; bandindex++ )
	{
		float energy, historicalAverage, ratio;
		int channel, weight, index;
		list_element* curr;
		historicalAverage = energy = 0;
		weight = 0;
		
		end = powf( 512.0,  ( bandindex + 1 ) / FREQUENCYBANDS );

		/*
		 * "Instant" energy.
		 */
		for( index = start; index < end; index++ )
			for( channel = 0; channel < renderData->numSpectrumChannels; channel++ )
			{
				energy += renderData->spectrumData[ channel ][ index ];
				weight++;
			}
		if( energy ) energy /= weight;
		
		/*
		 * "Historical" energy.
		 */
		historicalAverage = vPD->energyAggregate[ bandindex ] / vPD->energyCount[ bandindex ];

		/*
		 * Comparisons.
		 */
		ratio = energy / historicalAverage;
		if( energy > historicalAverage + MINPEAK && ratio > SENSITIVITY && ratio > bestratio )
		{
			bestratio = ratio;
			vPD->motorSpeed = 255 - ( ( bandindex + 1 ) / FREQUENCYBANDS ) * FALLOFF;
		}
	
		/* 
		 * Storage of the "Instant" record in the history buffer.
		 */
		if( vPD->energyCount[ bandindex ] >= RETAINSAMPLES )
		{
			list_element* deadnode = vPD->energyHistory[ bandindex ];
			vPD->energyAggregate[ bandindex ] -= deadnode->value;
			vPD->energyHistory[ bandindex ] = deadnode->next;
			free(deadnode);
			--vPD->energyCount[ bandindex ];
		}

		curr = vPD->energyHistory[ bandindex ];		
		while(curr->next != nil)
		{
			curr = curr->next;
		}
		curr->next = (list_element*)malloc(sizeof(list_element));
		curr = curr->next;
		curr->next = nil;
		curr->value = energy;
		vPD->energyAggregate[ bandindex ] += energy;
		++vPD->energyCount[ bandindex ];
	}
	
	/*
	 * Decay.
	 */
	if( bestratio == 0 )
	{
		if( vPD->motorSpeed <= DECAY ) vPD->motorSpeed = 0;
		else vPD->motorSpeed -= DECAY;	
	}
}

/*
 * Just a flood fill of the screen.  Done in reds if there is no vibrator connected,
 * or greys if one is.  The only complication here is that the rectangle is QuickDraw,
 * not Quartz, and so needs to be inverted within the dimensions of the viewport.
 */

static void UpdateScreen( VisualPluginData *vPD )
{
#if TARGET_OS_MAC
	float motorcolour = vPD->motorSpeed / 255.0;
	Rect *drawrect = &vPD->destRect;
	CGContextRef cgcontext;
	Rect bounds;
	CGRect blob;	
	
	GetPortBounds( vPD->destPort, &bounds );
	blob = CGRectMake( bounds.left, bounds.bottom - drawrect->bottom, bounds.right - bounds.left, drawrect->bottom - drawrect->top );
	
	QDBeginCGContext( vPD->destPort, &cgcontext );
	
		if( vPD->hasVibe == false ) CGContextSetRGBFillColor( cgcontext, motorcolour, 0, 0, 1 );
		else CGContextSetRGBFillColor( cgcontext, motorcolour, motorcolour, motorcolour, 1 );	
		CGContextFillRect( cgcontext, blob );
	
	QDEndCGContext( vPD->destPort, &cgcontext );
#else
	{
		RECT	srcRect;
		HBRUSH	hBrush;
		HDC		hdc;
		int motorcolour = vPD->motorSpeed;
		srcRect.left = vPD->destRect.left;
		srcRect.top = vPD->destRect.top;
		srcRect.right = vPD->destRect.right;
		srcRect.bottom = vPD->destRect.bottom;
		
		hdc = GetDC(vPD->destPort);		
		if (!(vPD->hasVibe))
			hBrush = CreateSolidBrush(RGB((UInt16)motorcolour,0,0));
		else
			hBrush = CreateSolidBrush(RGB((UInt16)motorcolour, (UInt16)motorcolour, (UInt16)motorcolour));
		FillRect(hdc, &srcRect, hBrush);
		DeleteObject(hBrush);
		ReleaseDC(vPD->destPort, hdc);
	}
#endif
}

static void SetupDevice( VisualPluginData *vPD )
{
	if(trancevibe_open(&(vPD->tv), 0) < 0) return;
	vPD->hasVibe = true;	
}

/*
 * Set the speeds of the vibrators.
 */
static void SetSpeed( VisualPluginData *vPD )
{		
	if( vPD->hasVibe == false || vPD->tv == nil) return;
	trancevibe_set_speed(vPD->tv, vPD->motorSpeed, 10);
}

/*
 * Clean up all our USB interface handles, etc.
 */
static void CleanupDevice( VisualPluginData *vPD )
{
	if( vPD->hasVibe == false ) return;
	vPD->motorSpeed = 0;
	SetSpeed( vPD );
	trancevibe_close(vPD->tv);
	vPD->hasVibe = false;
}

/*
 * Main entry point is here, not much to see though.
 */

#if TARGET_OS_WIN32
IMPEXP OSStatus MAIN(OSType message,PluginMessageInfo *messageInfo,void *refCon)
#else
OSStatus iTunesPluginMainMachO(OSType message,PluginMessageInfo *messageInfo,void *refCon)
#endif
{
	OSStatus status;
	switch( message )
	{
		case kPluginInitMessage:
			status = RegisterVisualPlugin( messageInfo );
			break;
			
		case kPluginCleanupMessage:
			status = noErr;
			break;
			
		default:
			status = unimpErr;
			break;
	}
	return status;
}
