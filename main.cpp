/*
    File: main.cpp
Abstract: n/a
 Version: 1.0.1

Disclaimer: IMPORTANT:  This Apple software is supplied to you by Apple
Inc. ("Apple") in consideration of your agreement to the following
terms, and your use, installation, modification or redistribution of
this Apple software constitutes acceptance of these terms.  If you do
not agree with these terms, please do not use, install, modify or
redistribute this Apple software.

In consideration of your agreement to abide by the following terms, and
subject to these terms, Apple grants you a personal, non-exclusive
license, under Apple's copyrights in this original Apple software (the
"Apple Software"), to use, reproduce, modify and redistribute the Apple
Software, with or without modifications, in source and/or binary forms;
provided that if you redistribute the Apple Software in its entirety and
without modifications, you must retain this notice and the following
text and disclaimers in all such redistributions of the Apple Software.
Neither the name, trademarks, service marks or logos of Apple Inc. may
be used to endorse or promote products derived from the Apple Software
without specific prior written permission from Apple.  Except as
expressly stated in this notice, no other rights or licenses, express or
implied, are granted by Apple herein, including but not limited to any
patent rights that may be infringed by your derivative works or by other
works in which the Apple Software may be incorporated.

The Apple Software is provided by Apple on an "AS IS" basis.  APPLE
MAKES NO WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION
THE IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND
OPERATION ALONE OR IN COMBINATION WITH YOUR PRODUCTS.

IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL
OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION,
MODIFICATION AND/OR DISTRIBUTION OF THE APPLE SOFTWARE, HOWEVER CAUSED
AND WHETHER UNDER THEORY OF CONTRACT, TORT (INCLUDING NEGLIGENCE),
STRICT LIABILITY OR OTHERWISE, EVEN IF APPLE HAS BEEN ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

Copyright (C) 2011 Apple Inc. All Rights Reserved.

*/

#include <CoreFoundation/CoreFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreMIDI/CoreMIDI.h>
#include <pthread.h>
#include <unistd.h>
#include <set>

#include "AUOutputBL.h"
#include "CAStreamBasicDescription.h"
#include "CAXException.h"
#include "CAHostTimeBase.h"

#include "CAAudioFileFormats.h"


static OSStatus LoadSMF(const char *filename, MusicSequence& sequence, MusicSequenceLoadFlags loadFlags);
static OSStatus GetSynthFromGraph (AUGraph & inGraph, AudioUnit &outSynth);
static OSStatus SetUpGraph (AUGraph &inGraph, UInt32 numFrames, Float64 &outSampleRate, bool isOffline);

static void WriteOutputFile (const char*	outputFilePath, 
					OSType			dataFormat, 
					Float64			srate, 
					MusicTimeStamp	sequenceLength, 
					bool			shouldPrint,
					AUGraph			inGraph,
					UInt32			numFrames,
					MusicPlayer		player);
			
static void PlayLoop (MusicPlayer &player, AUGraph &graph, MusicTimeStamp sequenceLength, bool shouldPrint, bool waitAtEnd);


static void str2OSType (const char * inString, OSType &outType)
{
	if (inString == NULL) {
		outType = 0;
		return;
	}
	
	size_t len = strlen(inString);
	if (len <= 4) {
		char workingString[5];
		
		workingString[4] = 0;
		workingString[0] = workingString[1] = workingString[2] = workingString[3] = ' ';
		memcpy (workingString, inString, strlen(inString));
		outType = 	*(workingString + 0) <<	24	|
					*(workingString + 1) <<	16	|
					*(workingString + 2) <<	8	|
					*(workingString + 3);
		return;
	}

	if (len <= 8) {
		if (sscanf (inString, "%lx", &outType) == 0) {
			printf ("* * Bad conversion for OSType\n"); 
			exit(1);
		}
		return;
	}
	printf ("* * Bad conversion for OSType\n"); 
	exit(1);
}


#define BANK_CMD 		"[-b /Path/To/Sound/Bank.dls]\n\t"
#define SMF_CHAN_CMD 	"[-c] Will Parse MIDI file into channels\n\t"
#define DISK_STREAM		"[-d] Turns disk streaming on\n\t"
#define MIDI_CMD 		"[-e] Use a MIDI Endpoint\n\t"
#define FILE_CMD		"[-f /Path/To/File.<EXT FOR FORMAT> 'data' srate] Create a stereo file where\n\t"
#define FILE_CMD_1			"\t\t 'data' is the data format (lpcm or a compressed type, like 'aac ')\n\t"
#define FILE_CMD_2			"\t\t srate is the sample rate\n\t"
#define NUM_FRAMES_CMD 	"[-i io Sample Size] default is 512\n\t"
#define NO_PRINT_CMD 	"[-n] Don't print\n\t"
#define PLAY_CMD 		"[-p] Play the Sequence\n\t"
#define START_TIME_CMD 	"[-s startTime-Beats]\n\t"
#define TRACK_CMD		"[-t trackIndex] Play specified track(s), e.g. -t 1 -t 2...(this is a one based index)\n\t"
#define WAIT_CMD		"[-w] Play for 10 seconds, then dispose all objects and wait at end\n\t"

#define SRC_FILE_CMD 	"/Path/To/File.mid"

static const char* usageStr = "Usage: PlaySequence\n\t" 
			BANK_CMD
			SMF_CHAN_CMD
			DISK_STREAM
			MIDI_CMD
			FILE_CMD
			FILE_CMD_1
			FILE_CMD_2
			NUM_FRAMES_CMD
			NO_PRINT_CMD
			PLAY_CMD
			START_TIME_CMD
			TRACK_CMD
			WAIT_CMD
			SRC_FILE_CMD;
							

UInt32 didOverload = 0;
UInt64	overloadTime = 0;
UInt64	startRunningTime;

Float32 maxCPULoad = .8;

int main (int argc, const char * argv[]) 
{
	if (argc == 1) {
		fprintf (stderr, "%s\n", usageStr);
		exit(0);
	}
	
	char* filePath = 0;
	bool shouldPlay = false;
	bool shouldSetBank = false;
    bool shouldUseMIDIEndpoint = false;
	bool shouldPrint = true;
	bool waitAtEnd = false;
	bool diskStream = false;
	
	OSType dataFormat = 0;
	Float64 srate = 0;
	const char* outputFilePath = 0;
	
	MusicSequenceLoadFlags	loadFlags = 0;
	
	char* bankPath = 0;
	Float32 startTime = 0;
	UInt32 numFrames = 512;
	
	std::set<int> trackSet;
	
	for (int i = 1; i < argc; ++i)
	{
		if (!strcmp ("-p", argv[i]))
		{
			shouldPlay = true;
		}
		else if (!strcmp ("-w", argv[i]))
		{
			waitAtEnd = true;
		}
		else if (!strcmp ("-d", argv[i]))
		{
			diskStream = true;
		}
		else if (!strcmp ("-b", argv[i])) 
		{
			shouldSetBank = true;
			if (++i == argc) goto malformedInput;
			bankPath = const_cast<char*>(argv[i]);
		}
		else if (!strcmp ("-n", argv[i]))
		{
			shouldPrint = false;
		}
		else if ((filePath == 0) && (argv[i][0] == '/' || argv[i][0] == '~'))
		{
			filePath = const_cast<char*>(argv[i]);
		}
		else if (!strcmp ("-s", argv[i])) 
		{
			if (++i == argc) goto malformedInput;
			sscanf (argv[i], "%f", &startTime);
		}
		else if (!strcmp ("-t", argv[i])) 
		{
			int index;
			if (++i == argc) goto malformedInput;
			sscanf (argv[i], "%d", &index);
			trackSet.insert(--index);
		}
		else if (!strcmp("-e", argv[i]))
        {
            shouldUseMIDIEndpoint = true;
        }
		else if (!strcmp("-c", argv[i]))
        {
            loadFlags = kMusicSequenceLoadSMF_ChannelsToTracks;
        }
        else if (!strcmp ("-i", argv[i])) 
		{
			if (++i == argc) goto malformedInput;
			sscanf (argv[i], "%ld", &numFrames);
		}
        else if (!strcmp ("-f", argv[i])) 
		{
			if (i + 3 >= argc) goto malformedInput;
			outputFilePath = argv[++i];
			str2OSType (argv[++i], dataFormat);
			sscanf (argv[++i], "%lf", &srate);
		}
		else
		{
malformedInput:
			fprintf (stderr, "%s\n", usageStr);
			exit (1);
		}
	}
	
	if (filePath == 0) {
		fprintf (stderr, "You have to specify a MIDI file to print or play\n");
		fprintf (stderr, "%s\n", usageStr);
		exit (1);
	}
	
	if (shouldUseMIDIEndpoint && outputFilePath) {
		printf ("can't write a file when you try to play out to a MIDI Endpoint\n");
		exit (1);
	}
	
	MusicSequence sequence;
	OSStatus result;
	
	ca_require_noerr (result = LoadSMF (filePath, sequence, loadFlags), fail);
			
	if (shouldPrint) 
		CAShow (sequence);
	
	if (shouldPlay)
	{
        AUGraph graph = 0;
        AudioUnit theSynth = 0;
		
		ca_require_noerr (result = MusicSequenceGetAUGraph (sequence, &graph), fail);
		ca_require_noerr (result = AUGraphOpen (graph), fail);     
		  
		ca_require_noerr (result = GetSynthFromGraph (graph, theSynth), fail);
		ca_require_noerr (result = AudioUnitSetProperty (theSynth,
										kAudioUnitProperty_CPULoad,
										kAudioUnitScope_Global, 0,
										&maxCPULoad, sizeof(maxCPULoad)), fail);

        if (shouldUseMIDIEndpoint) 
		{
			MIDIClientRef	theMidiClient;
			MIDIClientCreate(CFSTR("Play Sequence"), NULL, NULL, &theMidiClient);		
            
			ItemCount destCount = MIDIGetNumberOfDestinations();
            if (destCount == 0) {
                fprintf (stderr, "No MIDI Endpoints to play to.\n");
                exit(1);
            }
            
            ca_require_noerr (result = MusicSequenceSetMIDIEndpoint (sequence, MIDIGetDestination(0)), fail);
        } 
		else 
		{   
			if (shouldSetBank) {      
				CFURLRef soundBankURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8*)bankPath, strlen(bankPath), false);
								
				printf ("Setting Sound Bank:%s\n", bankPath);
					
				result = AudioUnitSetProperty (theSynth,
												kMusicDeviceProperty_SoundBankURL,
												kAudioUnitScope_Global, 0,
												&soundBankURL, sizeof(soundBankURL));
				CFRelease(soundBankURL);
				ca_require_noerr (result, fail);
			}

			if (diskStream) {
				UInt32 value = diskStream;
				ca_require_noerr (result = AudioUnitSetProperty (theSynth,
											kMusicDeviceProperty_StreamFromDisk,
											kAudioUnitScope_Global, 0,
											&value, sizeof(value)), fail);
			}

			if (outputFilePath) {
				// need to tell synth that is going to render a file.
				UInt32 value = 1;
				ca_require_noerr (result = AudioUnitSetProperty (theSynth,
												kAudioUnitProperty_OfflineRender,
												kAudioUnitScope_Global, 0,
												&value, sizeof(value)), fail);
			}
			
			ca_require_noerr (result = SetUpGraph (graph, numFrames, srate, (outputFilePath != NULL)), fail);
			
			if (shouldPrint) {
				printf ("Sample Rate: %.1f \n", srate);
				printf ("Disk Streaming is enabled: %c\n", (diskStream ? 'T' : 'F'));
			}
			
			ca_require_noerr (result = AUGraphInitialize (graph), fail);

            if (shouldPrint)
				CAShow (graph);
        }
        
		MusicPlayer player;
		ca_require_noerr (result = NewMusicPlayer (&player), fail);

		ca_require_noerr (result = MusicPlayerSetSequence (player, sequence), fail);

		// figure out sequence length
		UInt32 ntracks;
		ca_require_noerr(MusicSequenceGetTrackCount (sequence, &ntracks), fail);
		MusicTimeStamp sequenceLength = 0;
		bool shouldPrintTracks = shouldPrint && !trackSet.empty();
		if (shouldPrintTracks)
			printf ("Only playing specified tracks:\n\t");
		
		for (UInt32 i = 0; i < ntracks; ++i) {
			MusicTrack track;
			MusicTimeStamp trackLength;
			UInt32 propsize = sizeof(MusicTimeStamp);
			ca_require_noerr (result = MusicSequenceGetIndTrack(sequence, i, &track), fail);
			ca_require_noerr (result = MusicTrackGetProperty(track, kSequenceTrackProperty_TrackLength,
							&trackLength, &propsize), fail);
			if (trackLength > sequenceLength)
				sequenceLength = trackLength;
			
			if (!trackSet.empty() && (trackSet.find(i) == trackSet.end()))
			{
				Boolean mute = true;
				ca_require_noerr (result = MusicTrackSetProperty(track, kSequenceTrackProperty_MuteStatus, &mute, sizeof(mute)), fail);
			} 
			else if (shouldPrintTracks) {
				printf ("%d, ", int(i+1));
			}
		}
		if (shouldPrintTracks) 
			printf ("\n");
			
	// now I'm going to add 8 beats on the end for the reverb/long releases to tail off...
		sequenceLength += 8;
		
		ca_require_noerr (result = MusicPlayerSetTime (player, startTime), fail);
		
		ca_require_noerr (result = MusicPlayerPreroll (player), fail);
		
		if (shouldPrint) {
			printf ("Ready to play: %s, %.2f beats long\n\t<Enter> to continue: ", filePath, sequenceLength); 

			getc(stdin);
		}
		
		startRunningTime = CAHostTimeBase::GetCurrentTime ();
		
/*		if (waitAtEnd && graph)
			AUGraphStart(graph);
*/		
		ca_require_noerr (result = MusicPlayerStart (player), fail);
		
		if (outputFilePath) 
			WriteOutputFile (outputFilePath, dataFormat, srate, sequenceLength, shouldPrint, graph, numFrames, player);
		else
			PlayLoop (player, graph, sequenceLength, shouldPrint, waitAtEnd);
					
		ca_require_noerr (result = MusicPlayerStop (player), fail);
		if (shouldPrint) printf ("finished playing\n");

/*		if (waitAtEnd) {
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 10, false);
			if (graph)
				AUGraphStop(graph);
			if (shouldPrint) printf ("disposing\n");
		}
*/		
// this shows you how you should dispose of everything
		ca_require_noerr (result = DisposeMusicPlayer (player), fail);
		ca_require_noerr (result = DisposeMusicSequence(sequence), fail);
		// don't own the graph so don't dispose it (the seq owns it as we never set it ourselves, we just got it....)
	}
	else {
		ca_require_noerr (result = DisposeMusicSequence(sequence), fail);
	}
	
	while (waitAtEnd)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
		
    return 0;
	
fail:
	if (shouldPrint) printf ("Error = %ld\n", result);
	return result;
}

void PlayLoop (MusicPlayer &player, AUGraph &graph, MusicTimeStamp sequenceLength, bool shouldPrint, bool waitAtEnd)
{
	OSStatus result;
	int waitCounter = 0;
	while (1) {
		usleep (2 * 1000 * 1000);
		
		if (didOverload) {
			printf ("* * * * * %ld Overloads detected on device playing audio\n", didOverload);
			overloadTime = CAHostTimeBase::ConvertToNanos (overloadTime - startRunningTime);
			printf ("\tSeconds after start = %lf\n", double(overloadTime / 1000000000.));
			didOverload = 0;
		}

		if (waitAtEnd && ++waitCounter > 10) break;
		
		MusicTimeStamp time;
		ca_require_noerr (result = MusicPlayerGetTime (player, &time), fail);
					
		if (shouldPrint) {
			printf ("current time: %6.2f beats", time);
			if (graph) {
				Float32 load;
				ca_require_noerr (result = AUGraphGetCPULoad(graph, &load), fail);
				printf (", CPU load = %.2f%%\n", (load * 100.));
			} else
				printf ("\n"); //no cpu load on AUGraph - its not running - if just playing out to MIDI
		}
		
		if (time >= sequenceLength)
			break;
	}
	
	return;
fail:
	if (shouldPrint) printf ("Error = %ld\n", result);
	exit(1);
}

OSStatus GetSynthFromGraph (AUGraph& inGraph, AudioUnit& outSynth)
{	
	UInt32 nodeCount;
	OSStatus result = noErr;
	ca_require_noerr (result = AUGraphGetNodeCount (inGraph, &nodeCount), fail);
	
	for (UInt32 i = 0; i < nodeCount; ++i) 
	{
		AUNode node;
		ca_require_noerr (result = AUGraphGetIndNode(inGraph, i, &node), fail);

		AudioComponentDescription desc;
		ca_require_noerr (result = AUGraphNodeInfo(inGraph, node, &desc, 0), fail);
		
		if (desc.componentType == kAudioUnitType_MusicDevice) 
		{
			ca_require_noerr (result = AUGraphNodeInfo(inGraph, node, 0, &outSynth), fail);
			return noErr;
		}
	}
	
fail:		// didn't find the synth AU
	return -1;
}

void OverlaodListenerProc(	void *				inRefCon,
								AudioUnit			ci,
								AudioUnitPropertyID	inID,
								AudioUnitScope		inScope,
								AudioUnitElement	inElement)
{
	didOverload++;
	overloadTime = CAHostTimeBase::GetCurrentTime();
}


OSStatus SetUpGraph (AUGraph &inGraph, UInt32 numFrames, Float64 &sampleRate, bool isOffline)
{
	OSStatus result = noErr;
	AudioUnit outputUnit = 0;
	AUNode outputNode;
	
	// the frame size is the I/O size to the device
	// the device is going to run at a sample rate it is set at
	// so, when we set this, we also have to set the max frames for the graph nodes
	UInt32 nodeCount;
	ca_require_noerr (result = AUGraphGetNodeCount (inGraph, &nodeCount), home);

	for (int i = 0; i < (int)nodeCount; ++i) 
	{
		AUNode node;
		ca_require_noerr (result = AUGraphGetIndNode(inGraph, i, &node), home);

		AudioComponentDescription desc;
		AudioUnit unit;
		ca_require_noerr (result = AUGraphNodeInfo(inGraph, node, &desc, &unit), home);
		
		if (desc.componentType == kAudioUnitType_Output) 
		{
			if (outputUnit == 0) {
				outputUnit = unit;
				ca_require_noerr (result = AUGraphNodeInfo(inGraph, node, 0, &outputUnit), home);
				
				if (!isOffline) {
					// these two properties are only applicable if its a device we're playing too
					ca_require_noerr (result = AudioUnitSetProperty (outputUnit, 
													kAudioDevicePropertyBufferFrameSize, 
													kAudioUnitScope_Output, 0,
													&numFrames, sizeof(numFrames)), home);
				
					ca_require_noerr (result = AudioUnitAddPropertyListener (outputUnit, 
													kAudioDeviceProcessorOverload, 
													OverlaodListenerProc, 0), home);

					// if we're rendering to the device, then we render at its sample rate
					UInt32 theSize;
					theSize = sizeof(sampleRate);
					
					ca_require_noerr (result = AudioUnitGetProperty (outputUnit,
												kAudioUnitProperty_SampleRate,
												kAudioUnitScope_Output, 0,
												&sampleRate, &theSize), home);
				} else {
						// remove device output node and add generic output
					ca_require_noerr (result = AUGraphRemoveNode (inGraph, node), home);
					desc.componentSubType = kAudioUnitSubType_GenericOutput;
					ca_require_noerr (result = AUGraphAddNode (inGraph, &desc, &node), home);
					ca_require_noerr (result = AUGraphNodeInfo(inGraph, node, NULL, &unit), home);
					outputUnit = unit;
					outputNode = node;
					
					// we render the output offline at the desired sample rate
					ca_require_noerr (result = AudioUnitSetProperty (outputUnit,
												kAudioUnitProperty_SampleRate,
												kAudioUnitScope_Output, 0,
												&sampleRate, sizeof(sampleRate)), home);
				}
				// ok, lets start the loop again now and do it all...
				i = -1;
			}
		}
		else
		{
				// we only have to do this on the output side
				// as the graph's connection mgmt will propogate this down.
			if (outputUnit) {	
					// reconnect up to the output unit if we're offline
				if (isOffline && desc.componentType != kAudioUnitType_MusicDevice) {
					ca_require_noerr (result = AUGraphConnectNodeInput (inGraph, node, 0, outputNode, 0), home);
				}
				
				ca_require_noerr (result = AudioUnitSetProperty (unit,
											kAudioUnitProperty_SampleRate,
											kAudioUnitScope_Output, 0,
											&sampleRate, sizeof(sampleRate)), home);
			
			
			}
		}
		ca_require_noerr (result = AudioUnitSetProperty (unit, kAudioUnitProperty_MaximumFramesPerSlice,
												kAudioUnitScope_Global, 0,
												&numFrames, sizeof(numFrames)), home);
	}
	
home:
	return result;
}

OSStatus LoadSMF(const char *filename, MusicSequence& sequence, MusicSequenceLoadFlags loadFlags)
{
	OSStatus result = noErr;
    CFURLRef url = NULL;
	
	ca_require_noerr (result = NewMusicSequence(&sequence), home);
	
	url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8*)filename, strlen(filename), false);
	
	ca_require_noerr (result = MusicSequenceFileLoad (sequence, url, 0, loadFlags), home);
    	
home:
    if (url) CFRelease(url);
	return result;
}

#pragma mark -
#pragma mark Write Output File

bool TestFile (const char* fname, bool inDelete)
{
	// use this to determine if a file exists first
	FILE *f = fopen (fname, "r");
	if (f) {
		fclose (f);
		// wipe out the output file
		if (inDelete) {
			char str[1024];
			sprintf (str, "rm %s", fname);
			system(str);
		}
		return true;
	}
	return false;
}

void WriteOutputFile (const char*	outputFilePath, 
					OSType			dataFormat, 
					Float64			srate, 
					MusicTimeStamp	sequenceLength, 
					bool			shouldPrint,
					AUGraph			inGraph,
					UInt32			numFrames,
					MusicPlayer		player)
{
		// delete existing output  file
	TestFile (outputFilePath, true);
	OSStatus result = 0;
	UInt32 size;

	CAStreamBasicDescription outputFormat;
	outputFormat.mChannelsPerFrame = 2;
	outputFormat.mSampleRate = srate;
	outputFormat.mFormatID = dataFormat;
	
	AudioFileTypeID destFileType;
	CAAudioFileFormats::Instance()->InferFileFormatFromFilename (outputFilePath, destFileType);
	
	if (dataFormat == kAudioFormatLinearPCM) {
		outputFormat.mBytesPerPacket = outputFormat.mChannelsPerFrame * 2;
		outputFormat.mFramesPerPacket = 1;
		outputFormat.mBytesPerFrame = outputFormat.mBytesPerPacket;
		outputFormat.mBitsPerChannel = 16;
		
		if (destFileType == kAudioFileWAVEType)
			outputFormat.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger
								| kLinearPCMFormatFlagIsPacked;
		else
			outputFormat.mFormatFlags = kLinearPCMFormatFlagIsBigEndian
								| kLinearPCMFormatFlagIsSignedInteger
								| kLinearPCMFormatFlagIsPacked;
	} else {
		// use AudioFormat API to fill out the rest.
		size = sizeof(outputFormat);
		ca_require_noerr (result = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &size, &outputFormat), fail);
	}

	if (shouldPrint) {
		printf ("Writing to file: %s with format:\n* ", outputFilePath);
		outputFormat.Print();
	}
	
	CFURLRef url; url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)outputFilePath, strlen(outputFilePath), false);

	ExtAudioFileRef outfile;
	result = ExtAudioFileCreateWithURL(url, destFileType,&outputFormat, NULL, 0, &outfile);
	CFRelease (url);
	
	ca_require_noerr (result, fail);

	AudioUnit outputUnit;
	UInt32 nodeCount;
	ca_require_noerr (result = AUGraphGetNodeCount (inGraph, &nodeCount), fail);
	
	for (UInt32 i = 0; i < nodeCount; ++i) 
	{
		AUNode node;
		ca_require_noerr (result = AUGraphGetIndNode(inGraph, i, &node), fail);

		AudioComponentDescription desc;
		ca_require_noerr (result = AUGraphNodeInfo(inGraph, node, &desc, NULL), fail);
		
		if (desc.componentType == kAudioUnitType_Output) 
		{
			ca_require_noerr (result = AUGraphNodeInfo(inGraph, node, 0, &outputUnit), fail);
			break;
		}
	}

	{
		CAStreamBasicDescription clientFormat = CAStreamBasicDescription();
        ca_require_noerr (result = AudioUnitGetProperty(outputUnit,
                                                        kAudioUnitProperty_StreamFormat,
                                                        kAudioUnitScope_Output, 0,
                                                        &clientFormat, &size), fail);
		size = sizeof(clientFormat);
		ca_require_noerr (result = ExtAudioFileSetProperty(outfile, kExtAudioFileProperty_ClientDataFormat, size, &clientFormat), fail);
		
		{
			MusicTimeStamp currentTime;
			AUOutputBL outputBuffer (clientFormat, numFrames);
			AudioTimeStamp tStamp;
			memset (&tStamp, 0, sizeof(AudioTimeStamp));
			tStamp.mFlags = kAudioTimeStampSampleTimeValid;
			int i = 0;
			int numTimesFor10Secs = (int)(10. / (numFrames / srate));
			do {
				outputBuffer.Prepare();
				AudioUnitRenderActionFlags actionFlags = 0;
				ca_require_noerr (result = AudioUnitRender (outputUnit, &actionFlags, &tStamp, 0, numFrames, outputBuffer.ABL()), fail);

				tStamp.mSampleTime += numFrames;
				
				ca_require_noerr (result = ExtAudioFileWrite(outfile, numFrames, outputBuffer.ABL()), fail);	

				ca_require_noerr (result = MusicPlayerGetTime (player, &currentTime), fail);
				if (shouldPrint && (++i % numTimesFor10Secs == 0))
					printf ("current time: %6.2f beats\n", currentTime);
			} while (currentTime < sequenceLength);
		}
	}
	
// close
	ExtAudioFileDispose(outfile);

	return;

fail:
	printf ("Problem: %ld\n", result); 
	exit(1);
}
