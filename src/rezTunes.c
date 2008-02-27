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
 *  Ported to libtrancevibe by Kyle Machulis (kyle@nonpolynomial.com)
 *  http://www.nonpolynomial.com
 */

#include "iTunesVisualAPI.h"
#include "trancevibe.h"

#define kTVisualPluginName	"\014rezTunes"
#define	kTVisualPluginCreator	'hook'

#define kTVisualPluginMajorVersion 1
#define kTVisualPluginMinorVersion 1
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

typedef struct VisualPluginData {
	void				*appCookie;
	ITAppProcPtr		appProc;
	ITFileSpec			pluginFileSpec;
	CGrafPtr			destPort;
	Rect				destRect;
	OptionBits			destOptions;
	UInt32				destBitDepth;
	RenderVisualData	renderData;
	UInt32				renderTimeStampID;
	ITTrackInfo			trackInfo;
	ITStreamInfo		streamInfo;
	Boolean				playing;
	Boolean				running;
	Boolean				hasVibe;
	Boolean				padding[ 1 ];
	/*
	 * Things will break if the struct is not
	 * at least this big.  I'm not sure as to why.
	 */
	UInt8				motorSpeed;
	SInt32				volume;
	CFMutableArrayRef	vibeDevHandles;
	CFMutableArrayRef	vibeIntHandles;
	CFMutableArrayRef	energyHistory[ FREQUENCYBANDS ];
	trancevibe tv;
} VisualPluginData;


/*
 * Function Prototypes.
 */
extern OSStatus iTunesPluginMainMachO( OSType message, PluginMessageInfo *messageInfo, void *refCon );
static OSStatus VisualPluginHandler( OSType message, VisualPluginMessageInfo *messageInfo, void *refCon );
static OSStatus RegisterVisualPlugin( PluginMessageInfo *messageInfo );

static void MemClear( LogicalAddress dest, SInt32 length );

static void ProcessRenderData( VisualPluginData *vPD, const RenderVisualData *renderData );
static void UpdateScreen( VisualPluginData *vPD );

static void SetupDevice( 
VisualPluginData *vPD );
static void SetSpeed( VisualPluginData *vPD );
static void CleanupDevice( VisualPluginData *vPD );

/*
 * Main entry point is here, not much to see though.
 */
OSStatus iTunesPluginMainMachO( OSType message, PluginMessageInfo *messageInfo, void *refCon )
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

/*
 * Central messaging and dispatch function.
 */
static OSStatus VisualPluginHandler( OSType message, VisualPluginMessageInfo *messageInfo, void *refCon )
{
	VisualPluginData *vPD;
	OSStatus status;
	UInt8 oldSpeed;
	UInt8 i;

	vPD = ( VisualPluginData * ) refCon;
	oldSpeed = vPD->motorSpeed;
	
	status = noErr;
	
	switch( message )
	{
		/*
		 * Allocate a new structure, and start chasing down those vibrators.
		 */
		case kVisualPluginInitMessage:
		{
			int i = 0;
			
			vPD = ( VisualPluginData * ) NewPtrClear( sizeof( VisualPluginData ) );
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
			
			for( i = 0; i < FREQUENCYBANDS; i++ )
				vPD->energyHistory[ i ] = CFArrayCreateMutable( NULL, RETAINSAMPLES, &kCFTypeArrayCallBacks );
			
			/*
			 * These callbacks turn off reference count management in the
			 * array, which is important because these are just a group
			 * of pointers we're using.
			 */
			CFArrayCallBacks callbacks = { 0, NULL, NULL, NULL, NULL };				
			vPD->vibeDevHandles = CFArrayCreateMutable( 0, 0, &callbacks );
			vPD->vibeIntHandles = CFArrayCreateMutable( 0, 0, &callbacks );
			
			messageInfo->u.initMessage.options = 0;
			messageInfo->u.initMessage.refCon = (void*) vPD;
			status = PlayerGetPluginFileSpec( vPD->appCookie, vPD->appProc, &vPD->pluginFileSpec );
			SetupDevice(vPD);		
			break;
		}
			
		/*
		 * Cleanup.
		 */
		case kVisualPluginCleanupMessage:
			CleanupDevice( vPD );
			for( i = 0; i < FREQUENCYBANDS; i++ ) CFRelease( vPD->energyHistory[ i ] );
			if( vPD != nil ) DisposePtr( ( Ptr ) vPD );
			break;

		case kVisualPluginShowWindowMessage:
			vPD->destOptions = messageInfo->u.showWindowMessage.options;
			vPD->destPort = messageInfo->u.showWindowMessage.port;
			vPD->destRect = messageInfo->u.showWindowMessage.drawRect;
			vPD->running = true;
			UpdateScreen( vPD );
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
		
		case kVisualPluginEnableMessage:
		case kVisualPluginDisableMessage:
			break;

		default:
			status = unimpErr;
			break;
	}
	
	if( vPD->playing == false || vPD->running == false ) vPD->motorSpeed = 0;
	
	if( oldSpeed != vPD->motorSpeed && vPD->hasVibe == true ) SetSpeed( vPD );
	
	return status;	
}

static OSStatus RegisterVisualPlugin( PluginMessageInfo *messageInfo )
{
	OSStatus			status;
	PlayerMessageInfo	playerMessageInfo;
	Str255				pluginName = kTVisualPluginName;

	MemClear( &playerMessageInfo.u.registerVisualPluginMessage, sizeof( playerMessageInfo.u.registerVisualPluginMessage ) );
	BlockMoveData( (Ptr)&pluginName[0], (Ptr)&playerMessageInfo.u.registerVisualPluginMessage.name[0], pluginName[0] + 1 );
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

	playerMessageInfo.u.registerVisualPluginMessage.options					= kVisualProvidesUnicodeName;
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
		CFNumberRef energyRef;
		
		historicalAverage = energy = weight = 0;
		
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
		start = end;
		
		/*
		 * "Historical" energy.
		 */
		for( index = 0; index < CFArrayGetCount( vPD->energyHistory[ bandindex ] ); index++ )
		{
			float zog;
			CFNumberGetValue( CFArrayGetValueAtIndex( vPD->energyHistory[ bandindex ], index ), kCFNumberFloatType, &zog );
			historicalAverage += zog;
		}
		historicalAverage /= CFArrayGetCount( vPD->energyHistory[ bandindex ] );
		
		/* 
		 * Storage of the "Instant" record in the history buffer.
		 */
		energyRef = CFNumberCreate( kCFAllocatorDefault, kCFNumberFloatType, &energy );		
		if( CFArrayGetCount( vPD->energyHistory[ bandindex ] ) >= RETAINSAMPLES )
			CFArrayRemoveValueAtIndex( vPD->energyHistory[ bandindex ], RETAINSAMPLES - 1 );
		CFArrayInsertValueAtIndex( vPD->energyHistory[ bandindex ], 0, energyRef );
		CFRelease( energyRef );
		
		/*
		 * Comparisons.
		 */
		ratio = energy / historicalAverage;
		if( energy > historicalAverage + MINPEAK && ratio > SENSITIVITY && ratio > bestratio )
		{
		/* CFStringRef b = CFStringCreateWithFormat( 0, 0, CFSTR( "energy: %f, ratio was : %f\n" ), energy, ratio ); CFShow( b ); CFRelease( b ); */
			bestratio = ratio;
			vPD->motorSpeed = 255 - ( ( bandindex + 1 ) / FREQUENCYBANDS ) * FALLOFF;
		}
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
}

static void SetupDevice( VisualPluginData *vPD )
{
	trancevibe_open(&vPD->tv, 0);
	vPD->hasVibe = true;	
}

/*
 * Set the speeds of the vibrators.
 */
static void SetSpeed( VisualPluginData *vPD )
{		
	if( vPD->hasVibe == false ) return;
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