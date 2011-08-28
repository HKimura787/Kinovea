/*
Copyright © Joan Charmant 2008-2009.
joan.charmant@gmail.com 
 
This file is part of Kinovea.

Kinovea is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 
as published by the Free Software Foundation.

Kinovea is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Kinovea. If not, see http://www.gnu.org/licenses/.

*/

#include "VideoReaderFFMpeg.h"

using namespace System::Diagnostics;
using namespace System::Drawing;
using namespace System::Drawing::Drawing2D;
using namespace System::IO;
using namespace System::Runtime::InteropServices;

using namespace Kinovea::Video::FFMpeg;

VideoReaderFFMpeg::VideoReaderFFMpeg()
{
	// FFMpeg init.
	av_register_all();
	avcodec_init();
	avcodec_register_all();
	
	// Data init.
	m_bIsLoaded = false;
	m_iVideoStream = -1;
	m_iAudioStream = -1;
	m_iMetadataStream = -1;
	m_VideoInfo = VideoInfo::Empty;
	m_WorkingZone = VideoSection::Empty;
	m_TimestampInfo = TimestampInfo::Empty;
	
	VideoFrameDisposer^ disposer = gcnew VideoFrameDisposer(DisposeFrame);
	Cache = gcnew VideoFrameCache(disposer);
}
VideoReaderFFMpeg::~VideoReaderFFMpeg()
{
	this->!VideoReaderFFMpeg();
}
VideoReaderFFMpeg::!VideoReaderFFMpeg()
{
	if(m_bIsLoaded) 
		Close();
}
OpenVideoResult VideoReaderFFMpeg::Open(String^ _filePath)
{
	OpenVideoResult result = OpenVideoResult::Success;
	
	if(m_bIsLoaded) 
		Close();

	m_VideoInfo.FilePath = _filePath;

	do
	{
		// Open file and get info on format (muxer).
		AVFormatContext* pFormatCtx = nullptr;
		char* pszFilePath = static_cast<char *>(Marshal::StringToHGlobalAnsi(_filePath).ToPointer());
		if(av_open_input_file(&pFormatCtx, pszFilePath , NULL, 0, NULL) != 0)
		{
			result = OpenVideoResult::FileNotOpenned;
			log->Debug("The file could not be openned. (Wrong path or not a video/image.)");
			break;
		}
		Marshal::FreeHGlobal(safe_cast<IntPtr>(pszFilePath));
		
		// Info on streams.
		if(av_find_stream_info(pFormatCtx) < 0 )
		{
			result = OpenVideoResult::StreamInfoNotFound;
			log->Debug("The streams Infos were not Found.");
			break;
		}
		
		// Check for muxed KVA.
		m_iMetadataStream = GetFirstStreamIndex(pFormatCtx, AVMEDIA_TYPE_SUBTITLE);
		if(m_iMetadataStream >= 0)
		{
			AVMetadataTag* pMetadataTag = av_metadata_get(pFormatCtx->streams[m_iMetadataStream]->metadata, "language", nullptr, 0);
			if( (pFormatCtx->streams[m_iMetadataStream]->codec->codec_id != CODEC_ID_TEXT) ||
				(pMetadataTag == nullptr) ||
				(strcmp((char*)pMetadataTag->value, "XML") != 0))
			{
				log->Debug("Subtitle stream found, but not analysis meta data: ignored.");
				m_iMetadataStream = -1;
			}
		}

		// Video stream.
		if( (m_iVideoStream = GetFirstStreamIndex(pFormatCtx, AVMEDIA_TYPE_VIDEO)) < 0 )
		{
			result = OpenVideoResult::VideoStreamNotFound;
			log->Debug("No Video stream found in the file. (File is audio only, or video stream is broken.)");
			break;
		}

		// Codec
		AVCodec* pCodec = nullptr;
		AVCodecContext* pCodecCtx = pFormatCtx->streams[m_iVideoStream]->codec;
		m_VideoInfo.IsCodecMpeg2 = (pCodecCtx->codec_id == CODEC_ID_MPEG2VIDEO);
		if( (pCodec = avcodec_find_decoder(pCodecCtx->codec_id)) == nullptr)
		{
			result = OpenVideoResult::CodecNotFound;
			log->Debug("No suitable codec to decode the video. (Worse than an unsupported codec.)");
			break;
		}

		if(avcodec_open(pCodecCtx, pCodec) < 0)
		{
			result = OpenVideoResult::CodecNotOpened;
			log->Debug("Codec could not be openned. (Codec known, but not supported yet.)");
			break;
		}

		// The fundamental unit of time in Kinovea is the timebase of the file.
		// The timebase is the unit of time (in seconds) in which the timestamps are represented.
		m_VideoInfo.AverageTimeStampsPerSeconds = (double)pFormatCtx->streams[m_iVideoStream]->time_base.den / (double)pFormatCtx->streams[m_iVideoStream]->time_base.num;
		double fAvgFrameRate = 0.0;
		if(pFormatCtx->streams[m_iVideoStream]->avg_frame_rate.den != 0)
			fAvgFrameRate = (double)pFormatCtx->streams[m_iVideoStream]->avg_frame_rate.num / (double)pFormatCtx->streams[m_iVideoStream]->avg_frame_rate.den;

		if(pFormatCtx->start_time > 0)
			m_VideoInfo.FirstTimeStamp = (int64_t)((double)((double)pFormatCtx->start_time / (double)AV_TIME_BASE) * m_VideoInfo.AverageTimeStampsPerSeconds);
		else
			m_VideoInfo.FirstTimeStamp = 0;
	
		if(pFormatCtx->duration > 0)
			m_VideoInfo.DurationTimeStamps = (int64_t)((double)((double)pFormatCtx->duration/(double)AV_TIME_BASE)*m_VideoInfo.AverageTimeStampsPerSeconds);
		else
			m_VideoInfo.DurationTimeStamps = 0;

		if(m_VideoInfo.DurationTimeStamps <= 0)
		{
			result = OpenVideoResult::StreamInfoNotFound;
			log->Debug("Duration info not found.");
			break;
		}
		
		// Average FPS. Based on the following sources:
		// - libav in stream info (already in fAvgFrameRate).
		// - libav in container or stream with duration in frames or microseconds (Rarely available but valid if so).
		// - stream->time_base	(Often KO, like 90000:1, expresses the timestamps unit)
		// - codec->time_base (Often OK, but not always).
		// - some ad-hoc special cases.
		int iTicksPerFrame = pCodecCtx->ticks_per_frame;
		m_VideoInfo.FramesPerSeconds = 0;
		if(fAvgFrameRate != 0)
		{
			m_VideoInfo.FramesPerSeconds = fAvgFrameRate;
			log->Debug("Average Fps estimation method: libav.");
		}
		else
		{
			// 1.a. Durations
			if( (pFormatCtx->streams[m_iVideoStream]->nb_frames > 0) && (pFormatCtx->duration > 0))
			{	
				m_VideoInfo.FramesPerSeconds = ((double)pFormatCtx->streams[m_iVideoStream]->nb_frames * (double)AV_TIME_BASE)/(double)pFormatCtx->duration;

				if(iTicksPerFrame > 1)
					m_VideoInfo.FramesPerSeconds /= iTicksPerFrame;
				
				log->Debug("Average Fps estimation method: Durations.");
			}
			else
			{
				// 1.b. stream->time_base, consider invalid if >= 1000.
				m_VideoInfo.FramesPerSeconds = (double)pFormatCtx->streams[m_iVideoStream]->time_base.den / (double)pFormatCtx->streams[m_iVideoStream]->time_base.num;
				
				if(m_VideoInfo.FramesPerSeconds < 1000)
				{
					if(iTicksPerFrame > 1)
						m_VideoInfo.FramesPerSeconds /= iTicksPerFrame;		

					log->Debug("Average Fps estimation method: Stream timebase.");
				}
				else
				{
					// 1.c. codec->time_base, consider invalid if >= 1000.
					m_VideoInfo.FramesPerSeconds = (double)pCodecCtx->time_base.den / (double)pCodecCtx->time_base.num;

					if(m_VideoInfo.FramesPerSeconds < 1000)
					{
						if(iTicksPerFrame > 1)
							m_VideoInfo.FramesPerSeconds /= iTicksPerFrame;
						
						log->Debug("Average Fps estimation method: Codec timebase.");
					}
					else if (m_VideoInfo.FramesPerSeconds == 30000)
					{
						m_VideoInfo.FramesPerSeconds = 29.97;
						log->Debug("Average Fps estimation method: special case detection (30000:1 -> 30000:1001).");
					}
					else if (m_VideoInfo.FramesPerSeconds == 25000)
					{
						m_VideoInfo.FramesPerSeconds = 24.975;
						log->Debug("Average Fps estimation method: special case detection (25000:1 -> 25000:1001).");
					}
					else
					{
						// Detection failed. Force to 25fps.
						m_VideoInfo.FramesPerSeconds = 25;
						log->Debug("Average Fps estimation method: Estimation failed. Fps will be forced to : " + m_VideoInfo.FramesPerSeconds);
					}
				}
			}
		}
		log->Debug("Ticks per frame: " + iTicksPerFrame);

		m_VideoInfo.FrameIntervalMilliseconds = (double)1000/m_VideoInfo.FramesPerSeconds;
		m_VideoInfo.AverageTimeStampsPerFrame = (int64_t)Math::Round(m_VideoInfo.AverageTimeStampsPerSeconds / m_VideoInfo.FramesPerSeconds);
		
		m_WorkingZone = VideoSection(
			m_VideoInfo.FirstTimeStamp, 
			m_VideoInfo.FirstTimeStamp + m_VideoInfo.DurationTimeStamps - m_VideoInfo.AverageTimeStampsPerFrame);
		Cache->SetWorkingZoneSentinels(m_WorkingZone);

		// Image size
		m_VideoInfo.OriginalSize = Size(pCodecCtx->width, pCodecCtx->height);
		
		if(pCodecCtx->sample_aspect_ratio.num != 0 && pCodecCtx->sample_aspect_ratio.num != pCodecCtx->sample_aspect_ratio.den)
		{
			// Anamorphic video, non square pixels.
			log->Debug("Display Aspect Ratio type: Anamorphic");
			if(pCodecCtx->codec_id == CODEC_ID_MPEG2VIDEO)
			{
				// If MPEG, sample_aspect_ratio is actually the DAR...
				// Reference for weird decision tree: mpeg12.c at mpeg_decode_postinit().
				double fDisplayAspectRatio = (double)pCodecCtx->sample_aspect_ratio.num / (double)pCodecCtx->sample_aspect_ratio.den;
				m_VideoInfo.PixelAspectRatio = ((double)pCodecCtx->height * fDisplayAspectRatio) / (double)pCodecCtx->width;

				if(m_VideoInfo.PixelAspectRatio < 1.0f)
					m_VideoInfo.PixelAspectRatio = fDisplayAspectRatio;
			}
			else
			{
				m_VideoInfo.PixelAspectRatio = (double)pCodecCtx->sample_aspect_ratio.num / (double)pCodecCtx->sample_aspect_ratio.den;
			}	
			
			m_VideoInfo.SampleAspectRatio = Fraction(pCodecCtx->sample_aspect_ratio.num, pCodecCtx->sample_aspect_ratio.den);
		}
		else
		{
			// Assume PAR=1:1.
			log->Debug("Display Aspect Ratio type: Square Pixels");
			m_VideoInfo.PixelAspectRatio = 1.0f;
		}

		SetDecodingSize(Options->ImageAspectRatio);
		
		m_pFormatCtx = pFormatCtx;
		m_pCodecCtx	= pCodecCtx;
		m_bIsLoaded = true;		
		result = OpenVideoResult::Success;
		DumpInfo();
	}
	while(false);
	
	return result;
}
void VideoReaderFFMpeg::Close()
{
	// Unload the video and dispose unmanaged resources.

	if(!m_bIsLoaded)
		return;

	m_bIsLoaded = false;
	m_WorkingZone = VideoSection::Empty;
	m_VideoInfo = VideoInfo::Empty;
	m_TimestampInfo = TimestampInfo::Empty;
	
	Cache->Clear();
	m_bIsCaching = false;

	if(m_pCodecCtx != nullptr)
		avcodec_close(m_pCodecCtx);
	
	if(m_pFormatCtx != nullptr)
		av_close_input_file(m_pFormatCtx);

	m_iVideoStream = -1;
	m_iAudioStream = -1;
	m_iMetadataStream = -1;
}
bool VideoReaderFFMpeg::MoveNext(bool _synchronous)
{
	if(!m_bIsLoaded)
		return false;

	// Currently always synchronous for dev.
	// When async, we shouldn't even bother to check if next is available.
	if(_synchronous || true)
	{
		if(!Cache->HasNext)
		{ 
			// The background thread should be blocked not to interfere.
			ReadFrame(-1, 1);
		}
	}

#ifdef INSTRUMENTATION	
	if(!Cache->Empty)
		log->DebugFormat("[{0}] - Memory: {1:0,0} bytes", Cache->Current->Timestamp, Process::GetCurrentProcess()->PrivateMemorySize64);
#endif

	return Cache->MoveNext();
}
bool VideoReaderFFMpeg::MoveTo(int64_t _timestamp)
{
	if(!m_bIsLoaded)
		return false;
	
	int64_t target = _timestamp;
	if(!Cache->Contains(target))
	{
		// Out of segment jump. 
		// Unless it's a rollover, we can't keep the segment contiguous so clear the cache.
		if(target != m_WorkingZone.Start || !Cache->Contains(m_WorkingZone.End))
			Cache->Clear();
		
		ReadFrame(target, 1);
		target = m_TimestampInfo.CurrentTimestamp;
	}

#ifdef INSTRUMENTATION	
	if(!Cache->Empty)
		log->DebugFormat("[{0}] - Memory: {1:0,0} bytes", Cache->Current->Timestamp, Process::GetCurrentProcess()->PrivateMemorySize64);
#endif

	return Cache->MoveTo(target);
}
String^ VideoReaderFFMpeg::ReadMetadata()
{
	if(m_iMetadataStream < 0)
		return "";
	
	String^ metadata = "";
	bool done = false;
	do
	{
		AVPacket InputPacket;
		if((av_read_frame( m_pFormatCtx, &InputPacket)) < 0)
			break;
		
		if(InputPacket.stream_index != m_iMetadataStream)
			continue;

		metadata = gcnew String((char*)InputPacket.data);
		done = true;
	}
	while(!done);
	
	// Back to start.
	avformat_seek_file(m_pFormatCtx, m_iVideoStream, 0, 0, 0, AVSEEK_FLAG_BACKWARD); 
	
	return metadata;
}

bool VideoReaderFFMpeg::ChangeAspectRatio(ImageAspectRatio _ratio)
{
	// Decoding thread should be stopped at this point.
	Options->ImageAspectRatio = _ratio;
	SetDecodingSize(_ratio);
	Cache->Clear();
	return true;
}
bool VideoReaderFFMpeg::ChangeDeinterlace(bool _deint)
{
	// Decoding thread should be stopped at this point.
	Options->Deinterlace = _deint;
	Cache->Clear();
	return true;
}
// ---------- Private --------------

void VideoReaderFFMpeg::DisposeFrame(VideoFrame^ _frame)
{
	// Dispose the Bitmap and the native buffer.
	// The pointer to the native buffer was stored in the Tag property.
	IntPtr^ ptr = dynamic_cast<IntPtr^>(_frame->Image->Tag);
	
	delete _frame->Image;
	
	if(ptr != nullptr)
	{
		uint8_t* pBuf = (uint8_t*)ptr->ToPointer();
		delete [] pBuf;
	}
}
ReadResult VideoReaderFFMpeg::ReadFrame(int64_t _iTimeStampToSeekTo, int _iFramesToDecode)
{
	//---------------------------------------------------------------------
	// Reads a frame and adds it to the frame cache.
	// This function works either for MoveTo or MoveNext type of requests.
	// It decodes as many frames as needed to reach the target timestamp 
	// or the number of frames to decode. Seeks backwards if needed.
	//---------------------------------------------------------------------

	ReadResult result = ReadResult::Success;
	int	iFramesToDecode = _iFramesToDecode;
	int64_t iTargetTimeStamp = _iTimeStampToSeekTo;
	bool seeking = false;

	if(!m_bIsLoaded) 
		return ReadResult::MovieNotLoaded;

	// Find the proper target and number of frames to decode.
	if(_iFramesToDecode < 0)
	{
		// Negative move. Compute seek target.
		iTargetTimeStamp = Cache->Current->Timestamp + (_iFramesToDecode * m_VideoInfo.AverageTimeStampsPerFrame);
		if(iTargetTimeStamp < 0)
			iTargetTimeStamp = 0;
	}

	if(iTargetTimeStamp >= 0)
	{	
		seeking = true;
		iFramesToDecode = 1; // We'll use the target timestamp anyway.

		// AVSEEK_FLAG_BACKWARD -> goes to first I-Frame before target.
		// Then we'll need to decode frame by frame until the target is reached.
		int iSeekRes = avformat_seek_file(
			m_pFormatCtx, 
			m_iVideoStream, 
			0, 
			iTargetTimeStamp, 
			iTargetTimeStamp + (int64_t)m_VideoInfo.AverageTimeStampsPerSeconds,
			AVSEEK_FLAG_BACKWARD);
		
		avcodec_flush_buffers( m_pFormatCtx->streams[m_iVideoStream]->codec);
		m_TimestampInfo = TimestampInfo::Empty;
		
		if(iSeekRes < 0)
			log->ErrorFormat("Error during seek: {0}. Target was:[{1}]", iSeekRes, iTargetTimeStamp);
	}
		
	// Allocate 2 AVFrames, one for the raw decoded frame and one for deinterlaced/rescaled/converted frame.
	AVFrame* pDecodingAVFrame = avcodec_alloc_frame();
	AVFrame* pFinalAVFrame = avcodec_alloc_frame();

	// The buffer holding the actual frame data.
	int iSizeBuffer = avpicture_get_size(m_PixelFormatFFmpeg, m_VideoInfo.DecodingSize.Width, m_VideoInfo.DecodingSize.Height);
	uint8_t* pBuffer = iSizeBuffer > 0 ? new uint8_t[iSizeBuffer] : nullptr;

	if(pDecodingAVFrame == nullptr || pFinalAVFrame == nullptr || pBuffer == nullptr)
		return ReadResult::MemoryNotAllocated;

	// Assigns appropriate parts of buffer to image planes in the AVFrame.
	avpicture_fill((AVPicture *)pFinalAVFrame, pBuffer , m_PixelFormatFFmpeg, m_VideoInfo.DecodingSize.Width, m_VideoInfo.DecodingSize.Height);

	m_TimestampInfo.CurrentTimestamp = Cache->Current == nullptr ? -1 : Cache->Current->Timestamp;
	
	// Reading/Decoding loop
	bool done = false;
	bool bFirstPass = true;
	int iReadFrameResult;
	int iFrameFinished = 0;
	int	iFramesDecoded	= 0;
	do
	{
		// FFMpeg also has an internal buffer to cope with B-Frames entanglement.
		// The DTS/PTS announced is actually the one of the last frame that was put in the buffer by av_read_frame,
		// it is *not* the one of the frame that was extracted from the buffer by avcodec_decode_video.
		// To solve the DTS/PTS issue, we save the timestamps each time we find libav is buffering a frame.
		// And we use the previously saved timestamps.
		// Ref: http://lists.mplayerhq.hu/pipermail/libav-user/2008-August/001069.html

		// Read next packet
		AVPacket InputPacket;
		iReadFrameResult = av_read_frame( m_pFormatCtx, &InputPacket);

		if(iReadFrameResult < 0)
		{
			// Reading error. We don't know if the error happened on a video frame or audio one.
			done = true;
			delete [] pBuffer;
			result = ReadResult::FrameNotRead;
			break;
		}

		if(InputPacket.stream_index != m_iVideoStream)
		{
			av_free_packet(&InputPacket);
			continue;
		}

		// Decode video packet. This is needed even if we're not on the final frame yet.
		// I-Frame data is kept internally by ffmpeg and will need it to build the final frame.
		avcodec_decode_video2(m_pCodecCtx, pDecodingAVFrame, &iFrameFinished, &InputPacket);
		
		if(iFrameFinished == 0)
		{
			// Buffering frame. libav just read a I or P frame that will be presented later.
			// (But which was necessary to get now to decode a coming B frame.)
			SetTimestampFromPacket(InputPacket.dts, InputPacket.pts, false);
			av_free_packet(&InputPacket);
			continue;
		}

		// Update positions.
		SetTimestampFromPacket(InputPacket.dts, InputPacket.pts, true);

		if(seeking && bFirstPass && iTargetTimeStamp >= 0 && m_TimestampInfo.CurrentTimestamp > iTargetTimeStamp)
		{
			// If the current ts is already after the target, we are dealing with this kind of files
			// where the seek doesn't work as advertised. We'll seek back again further,
			// and then decode until we get to it.
			
			// Do this only once.
			bFirstPass = false;
			
			// For some files, one additional second back is not enough. The seek is wrong by up to 4 seconds.
			// We also allow the target to go before 0.
			int iSecondsBack = 4;
			int64_t iForceSeekTimestamp = iTargetTimeStamp - ((int64_t)m_VideoInfo.AverageTimeStampsPerSeconds * iSecondsBack);
			int64_t iMinTarget = System::Math::Min(iForceSeekTimestamp, (int64_t)0);
			
			// Do the seek.
			log->DebugFormat("[Seek] - First decoded frame [{0}] already after target [{1}]. Force seek {2} more seconds back to [{3}]", 
							m_TimestampInfo.CurrentTimestamp, iTargetTimeStamp, iSecondsBack, iForceSeekTimestamp);
			
			avformat_seek_file(m_pFormatCtx, m_iVideoStream, iMinTarget , iForceSeekTimestamp, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD); 
			avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

			// Free the packet that was allocated by av_read_frame
			av_free_packet(&InputPacket);

			// Loop back to restart decoding frames until we get to the target.
			continue;
		}

		bFirstPass = false;
		iFramesDecoded++;

		//-------------------------------------------------------------------------------
		// If we're done, convert the image and store it into its final recipient.
		// - seek: if we reached the target timestamp.
		// - linear decoding: if we decoded the required number of frames.
		//-------------------------------------------------------------------------------
		if(	(seeking && m_TimestampInfo.CurrentTimestamp >= iTargetTimeStamp) ||
			(!seeking && iFramesDecoded >= iFramesToDecode))
		{
			done = true;

			if(seeking)
				log->DebugFormat("Seeking completed. Final position:[{0}]", m_TimestampInfo.CurrentTimestamp);

			// Deinterlace + rescale + convert pixel format.
			bool rescaled = RescaleAndConvert(
				pFinalAVFrame, 
				pDecodingAVFrame, 
				m_VideoInfo.DecodingSize.Width, 
				m_VideoInfo.DecodingSize.Height, 
				m_PixelFormatFFmpeg,
				Options->Deinterlace);

			if(!rescaled)
			{
				delete [] pBuffer;
				result = ReadResult::ImageNotConverted;
			}
			
			try
			{
				// Import ffmpeg buffer into a .NET bitmap.
				int imageStride = pFinalAVFrame->linesize[0];
				IntPtr scan0 = IntPtr((void*)pFinalAVFrame->data[0]); 
				Bitmap^ bmp = gcnew Bitmap(m_VideoInfo.DecodingSize.Width, m_VideoInfo.DecodingSize.Height, imageStride, DecodingPixelFormat, scan0);
				
				// Store a pointer to the native buffer inside the Bitmap.
				// We'll be asked to free this resource later when the frame is not used anymore.
				// It is boxed inside an Object so we can extract it in a type-safe way.
				IntPtr^ boxedPtr = gcnew IntPtr((void*)pBuffer);
				bmp->Tag = boxedPtr;
				
				// Construct the VideoFrame and push it to cache.
				VideoFrame^ vf = gcnew VideoFrame();
				vf->Image = bmp;
				vf->Timestamp = m_TimestampInfo.CurrentTimestamp;
				Cache->Add(vf);
			}
			catch(Exception^ exp)
			{
				delete [] pBuffer;
				result = ReadResult::ImageNotConverted;
				log->Error("Error while converting AVFrame to Bitmap.");
				log->Error(exp);
			}
		}
		
		// Free the packet that was allocated by av_read_frame
		av_free_packet(&InputPacket);
	}
	while(!done);
	
	// Free the AVFrames. (This will not deallocate the data buffers).
	av_free(pFinalAVFrame);
	av_free(pDecodingAVFrame);

#ifdef INSTRUMENTATION	
	if(!Cache->Empty)
		log->DebugFormat("[{0}] - Memory: {1:0,0} bytes", Cache->Current->Timestamp, Process::GetCurrentProcess()->PrivateMemorySize64);
#endif

	return result;
}
void VideoReaderFFMpeg::SetTimestampFromPacket(int64_t _dts, int64_t _pts, bool _bDecoded)
{
	//---------------------------------------------------------------------------------------------------------
	// Try to guess the presentation timestamp of the packet we just read / decoded.
	// Presentation timestamps will be used everywhere for seeking, positioning, time calculations, etc.
	//
	// dts: decoding timestamp, 
	// pts: presentation timestamp, 
	// decoded: if libav finished to decode the frame or is just buffering.
	//
	// It must be noted that the timestamp given by libav is the timestamp of the frame it just read,
	// but the frame we will get from av_decode_video may come from its internal buffer and have a different timestamp.
	// Furthermore, some muxers do not fill the PTS value, and others only intermittently.
	// Kinovea prior to version 0.8.8 was using the DTS value as primary timestamp, which is wrong.
	//---------------------------------------------------------------------------------------------------------

	if(_pts == AV_NOPTS_VALUE || _pts < 0)
	{
		// Hum, too bad, the muxer did not specify the PTS for this packet.

		if(_bDecoded)
		{
			if(_dts == AV_NOPTS_VALUE || _dts < 0)
			{
				/*log->Debug(String::Format("Decoded - No value for PTS / DTS. Last known timestamp: {0}, Buffered ts if any: {1}", 
					(m_PrimarySelection->iLastDecodedPTS >= 0)?String::Format("{0}", m_PrimarySelection->iLastDecodedPTS):"None", 
					(m_PrimarySelection->iBufferedPTS < Int64::MaxValue)?String::Format("{0}", m_PrimarySelection->iBufferedPTS):"None"));*/

				if(m_TimestampInfo.BufferedPTS < Int64::MaxValue)
				{
					// No info but we know a frame was previously buffered, so it must be this one we took out.
					// Unfortunately, we don't know the timestamp of the frame that is being buffered now...
					m_TimestampInfo.CurrentTimestamp = m_TimestampInfo.BufferedPTS;
					m_TimestampInfo.BufferedPTS = Int64::MaxValue;
				}
				else if(m_TimestampInfo.LastDecodedPTS >= 0)
				{
					// No info but we know a frame was previously decoded, so it must be shortly after it.
					m_TimestampInfo.CurrentTimestamp = m_TimestampInfo.LastDecodedPTS + m_VideoInfo.AverageTimeStampsPerFrame;
					//log->Debug(String::Format("Current PTS estimation: {0}",	m_PrimarySelection->iCurrentTimeStamp));
				}
				else
				{
					// No info + never buffered, never decoded. This must be the first frame.
					m_TimestampInfo.CurrentTimestamp = 0;
					//log->Debug(String::Format("Setting current PTS to 0"));
				}
			}
			else
			{
				// DTS is given, but not PTS.
				// Either this file is not using PTS, or it just didn't fill it for this frame, no way to know...
				if(m_TimestampInfo.BufferedPTS < _dts)
				{
					// Argh. Comparing buffered frame PTS with read frame DTS ?
					// May work for files that only store DTS all along though.
					//log->Debug(String::Format("Decoded buffered frame - [{0}]", m_PrimarySelection->iBufferedPTS));
					//log->Debug(String::Format("Buffering - DTS:[{0}] - No PTS", _dts));
					m_TimestampInfo.CurrentTimestamp = m_TimestampInfo.BufferedPTS;	
					m_TimestampInfo.BufferedPTS = _dts;
				}
				else
				{
					//log->Debug(String::Format("Decoded (direct) - DTS:[{0}], No PTS", _dts));
					m_TimestampInfo.CurrentTimestamp = System::Math::Max((int64_t)0, _dts);
				}
			}

			m_TimestampInfo.LastDecodedPTS = m_TimestampInfo.CurrentTimestamp;
		}
		else
		{
			// Buffering a frame.
			// What if there is already something in the buffer ?
			// We should keep a queue of buffered frames and serve them back in order.
			if(_dts < 0)
			{ 
				//log->Debug(String::Format("Buffering (no decode) - No PTS, negative DTS"));
				
				// Hopeless situation. Let's reset the buffered frame timestamp,
				// The decode will rely on the last seen PTS from a decoded frame, if any.
				m_TimestampInfo.BufferedPTS = Int64::MaxValue;
			}
			else if(_dts == AV_NOPTS_VALUE)
			{
				//log->Debug(String::Format("Buffering (no decode) - No PTS, No DTS"));
				m_TimestampInfo.BufferedPTS = 0;
			}
			else
			{
				//log->Debug(String::Format("Buffering (no decode) - No PTS, DTS:[{0}]", _dts));
				m_TimestampInfo.BufferedPTS = _dts;
			}
		}
	}
	else
	{
		// PTS is given (nice).
		// We still need to check if there is something in the buffer, in which case
		// the decoded frame is in fact the one from the buffer.
		// (This may not even hold true, for H.264 and out of GOP reference.)
		if(_bDecoded)
		{
			if(m_TimestampInfo.BufferedPTS < _pts)
			{
				// There is something in the buffer with a timestamp prior to the one of the decoded frame.
				// That is probably the frame we got from libav.
				// The timestamp it presented us on the other hand, is the one it's being buffering.
				//log->Debug(String::Format("Decoded buffered frame - PTS:[{0}]", m_PrimarySelection->iBufferedPTS));
				//log->Debug(String::Format("Buffering - DTS:[{0}], PTS:[{1}]", _dts, _pts));
				
				m_TimestampInfo.CurrentTimestamp = m_TimestampInfo.BufferedPTS;
				m_TimestampInfo.BufferedPTS = _pts;
			}
			else
			{
				//log->Debug(String::Format("Decoded (direct) - DTS:[{0}], PTS:[{1}]", _dts, _pts));
				m_TimestampInfo.CurrentTimestamp = _pts;
			}

			m_TimestampInfo.LastDecodedPTS = m_TimestampInfo.CurrentTimestamp;
		}
		else
		{
			// What if there is already something in the buffer ?
			// We should keep a queue of buffered frame and serve them back in order.
			//log->Debug(String::Format("Buffering (no decode) -- DTS:[{0}], PTS:[{1}]", _dts, _pts));
			m_TimestampInfo.BufferedPTS = _pts;
		}
	}
}
VideoSummary^ VideoReaderFFMpeg::ExtractSummary(String^ _filePath, int _thumbs, int _width)
{
	// FIXME: a lot of code is duplicated with the regular open/read method.
	// Code needs to be cleaned up.

	List<Bitmap^>^ thumbs = gcnew List<Bitmap^>();
	bool isImage = false;
	bool hasKva = false;
	Size imageSize = Size::Empty;
	int64_t durationMilliseconds = 0;
	
	int64_t iIntervalTimestamps = 1;
	
	Bitmap^ bmp = nullptr;
	bool bGotPicture = false;
	bool bCodecOpened = false;

	AVFormatContext* pFormatCtx = nullptr;
	AVCodecContext* pCodecCtx = nullptr;

	do
	{
		char* cFilePath = static_cast<char *>(Marshal::StringToHGlobalAnsi(_filePath).ToPointer());

		// 1. Ouvrir le fichier et récupérer les infos sur le format.
		
		if(av_open_input_file(&pFormatCtx, cFilePath , NULL, 0, NULL) != 0)
		{
			log->Error("GetThumbnail Error : Input file not opened");
			break;
		}
		Marshal::FreeHGlobal(safe_cast<IntPtr>(cFilePath));
		
		// 2. Obtenir les infos sur les streams contenus dans le fichier.
		if(av_find_stream_info(pFormatCtx) < 0 )
		{
			log->Error("GetThumbnail Error : Stream infos not found");
			break;
		}

		// Look for embedded or companion analysis file.
		int iMetadataStreamIndex = GetFirstStreamIndex(pFormatCtx, AVMEDIA_TYPE_SUBTITLE);
		if(iMetadataStreamIndex >= 0)
		{
			AVMetadataTag* pMetadataTag = av_metadata_get(pFormatCtx->streams[iMetadataStreamIndex]->metadata, "language", nullptr, 0);
			if( (pFormatCtx->streams[iMetadataStreamIndex]->codec->codec_id == CODEC_ID_TEXT) &&
				(pMetadataTag != nullptr) &&
				(strcmp((char*)pMetadataTag->value, "XML") == 0))
			{
				hasKva = true;
			}
		}
		
		if(!hasKva)
		{
			// No analysis found within the file, check for a companion .kva.
			String^ kvaFile = String::Format("{0}\\{1}.kva", Path::GetDirectoryName(_filePath), Path::GetFileNameWithoutExtension(_filePath));
			hasKva = File::Exists(kvaFile);
		}

		// 3. Obtenir l'identifiant du premier stream vidéo.
		int iVideoStreamIndex = -1;
		if( (iVideoStreamIndex = GetFirstStreamIndex(pFormatCtx, AVMEDIA_TYPE_VIDEO)) < 0 )
		{
			log->Error("GetThumbnail Error : First video stream not found");
			break;
		}

		// 4. Obtenir un objet de paramètres du codec vidéo.
		AVCodec* pCodec = nullptr;
		pCodecCtx = pFormatCtx->streams[iVideoStreamIndex]->codec;
		if( (pCodec = avcodec_find_decoder(pCodecCtx->codec_id)) == nullptr)
		{
			log->Error("GetThumbnail Error : Decoder not found");
			break;
		}

		// 5. Ouvrir le Codec vidéo.
		if(avcodec_open(pCodecCtx, pCodec) < 0)
		{
			log->Error("GetThumbnail Error : Decoder not opened");
			break;
		}
		bCodecOpened = true;


		// TODO:
		// Fill up an InfosThumbnail object with data.
		// (Fixes anamorphic, unsupported width, compute length, etc.)

		// 5.b Compute duration in timestamps.
		double fAverageTimeStampsPerSeconds = (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.den / (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.num;
		if(pFormatCtx->duration > 0)
		{
			durationMilliseconds = pFormatCtx->duration / 1000;
			
			// Compute the interval in timestamps at which we will extract thumbs.
			int64_t iDurationTimeStamps = (int64_t)((double)((double)pFormatCtx->duration / (double)AV_TIME_BASE) * fAverageTimeStampsPerSeconds);
			iIntervalTimestamps = iDurationTimeStamps / _thumbs;
			isImage = (iDurationTimeStamps == 1);
		}
		else
		{
			// No duration infos, only get one pic.
			_thumbs = 1;
		}

		imageSize = Size(pCodecCtx->width, pCodecCtx->height);

		// 6. Allocate video frames, one for decoding, one to hold the picture after conversion.
		AVFrame* pDecodingFrameBuffer = avcodec_alloc_frame();
		AVFrame* pDecodedFrameBGR = avcodec_alloc_frame();

		if( (pDecodedFrameBGR != NULL) && (pDecodingFrameBuffer != NULL) )
		{
			// We ask for pictures already reduced in size to lighten GDI+ burden: max at _iPicWidth px width.
			// This also takes care of image size which are not multiple of 4.
			float fWidthRatio = (float)pCodecCtx->width / _width;
			int iDecodingWidth = _width;
			int iDecodingHeight = (int)((float)pCodecCtx->height / fWidthRatio);

			int iSizeBuffer = avpicture_get_size(m_PixelFormatFFmpeg, iDecodingWidth, iDecodingHeight);
			if(iSizeBuffer < 1)
			{
				log->Error("GetThumbnail Error : Frame buffer not allocated");
				break;
			}
			uint8_t* pBuffer = new uint8_t[iSizeBuffer];

			// Assign appropriate parts of buffer to image planes in pFrameBGR
			avpicture_fill((AVPicture *)pDecodedFrameBGR, pBuffer , m_PixelFormatFFmpeg, iDecodingWidth, iDecodingHeight);
			
			int iTotalReadFrames = 0;
			
			//-------------------
			// Read the first frame.
			//-------------------
			bool done = false;
			do
			{
				AVPacket	InputPacket;
				
				int iReadFrameResult = av_read_frame( pFormatCtx, &InputPacket);

				if(iReadFrameResult >= 0)
				{
					// Is this a packet from the video stream ?
					if(InputPacket.stream_index == iVideoStreamIndex)
					{
						// Decode video frame
						int	frameFinished;
						avcodec_decode_video2(pCodecCtx, pDecodingFrameBuffer, &frameFinished, &InputPacket);

						if(frameFinished)
						{
							iTotalReadFrames++;
							if(iTotalReadFrames > _thumbs - 1)
								done=true;

							SwsContext* pSWSCtx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, iDecodingWidth, iDecodingHeight, m_PixelFormatFFmpeg, SWS_FAST_BILINEAR, NULL, NULL, NULL); 
							sws_scale(pSWSCtx, pDecodingFrameBuffer->data, pDecodingFrameBuffer->linesize, 0, pCodecCtx->height, pDecodedFrameBGR->data, pDecodedFrameBGR->linesize); 
							sws_freeContext(pSWSCtx);

							try
							{
								IntPtr* ptr = new IntPtr((void*)pDecodedFrameBGR->data[0]); 
								Bitmap^ tmpBitmap = gcnew Bitmap( iDecodingWidth, iDecodingHeight, pDecodedFrameBGR->linesize[0], this->DecodingPixelFormat, *ptr );
							
								//---------------------------------------------------------------------------------
								// Dupliquer complètement, 
								// Bitmap.Clone n'est pas suffisant, on continue de pointer vers les mêmes données.
								//---------------------------------------------------------------------------------
								bmp = AForge::Imaging::Image::Clone(tmpBitmap, tmpBitmap->PixelFormat);
								thumbs->Add(bmp);
								delete tmpBitmap;
								tmpBitmap = nullptr;
								bGotPicture = true;
							}
							catch(Exception^)
							{
								log->Error("GetThumbnail Error : Bitmap creation failed");
								bmp = nullptr;
							}
							
							//-------------------------------------------
							// Seek to next image. 
							// Approximation : We don't care about first timestamp being greater than 0.	
							//-------------------------------------------
							if(iTotalReadFrames > 0 && iTotalReadFrames < _thumbs)
							{
								try
								{
									//log->Debug(String::Format("Jumping to {0} to extract thumbnail {1}.", iTotalReadFrames * iIntervalTimestamps, iTotalReadFrames+1));
									av_seek_frame(pFormatCtx, iVideoStreamIndex, (iTotalReadFrames * iIntervalTimestamps), AVSEEK_FLAG_BACKWARD);
									avcodec_flush_buffers(pFormatCtx->streams[iVideoStreamIndex]->codec);
								}
								catch(Exception^)
								{
									log->Error("GetThumbnail Error : Jumping to next extraction point failed.");
									done = true;
								}
							}
						}
						else
						{
							int iFrameNotFinished=1; // test pour debug
						}
					}
					else
					{
						// Not the first video stream.
						//Console::WriteLine("This is Stream #{0}, Loop until stream #{1}", InputPacket.stream_index, iVideoStreamIndex);
					}
					
					// Free the packet that was allocated by av_read_frame
					av_free_packet(&InputPacket);
				}
				else
				{
					// Reading error.
					done = true;
					log->Error("GetThumbnail Error : Frame reading failed");
				}
			}
			while(!done);
			
			// Clean Up
			delete []pBuffer;
			pBuffer = nullptr;
			
			av_free(pDecodingFrameBuffer);
			av_free(pDecodedFrameBGR);
		}
		else
		{
			// Not enough memory to allocate the frame.
			log->Error("GetThumbnail Error : AVFrame holders not allocated");
		}
	}
	while(false);

	if(bCodecOpened)
	{
		avcodec_close(pCodecCtx);
		//av_free(pCodecCtx);
	}

	if(pFormatCtx != nullptr)
	{
		av_close_input_file(pFormatCtx);
		//av_free(pFormatCtx);	
	}

	VideoSummary^ summary = gcnew VideoSummary(isImage, hasKva, imageSize, durationMilliseconds, thumbs);
	return summary;
}
bool VideoReaderFFMpeg::CanExtractToMemory(int64_t _iStartTimeStamp, int64_t _iEndTimeStamp, int _maxSeconds, int _maxMemory)
{
	// Check if the current selection could switch to analysis mode, according to the current settings.
	// _maxMemory is in Mib.
	
	// To be analyzable, both conditions must be met.
	int iDurationTimeStamps = (int)(_iEndTimeStamp - _iStartTimeStamp);
	double iDurationSeconds = (double)iDurationTimeStamps / m_InfosVideo->fAverageTimeStampsPerSeconds;
	
	int iFrameMemoryBytes = avpicture_get_size(m_PixelFormatFFmpeg, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
	double iFrameMemoryMegaBytes = (double)iFrameMemoryBytes  / 1048576;
	int iTotalFrames = (int)(iDurationTimeStamps / m_InfosVideo->iAverageTimeStampsPerFrame);
	int iDurationMemory = (int)((double)iTotalFrames * iFrameMemoryMegaBytes);
	
	bool result = false;
	if( (iDurationSeconds > 0) && (iDurationSeconds <= _maxSeconds) && (iDurationMemory <= _maxMemory))
	{
		result = true;
	}
    
	return result;
}
void VideoReaderFFMpeg::ExtractToMemory(int64_t _iStartTimeStamp, int64_t _iEndTimeStamp, bool _bForceReload)
{
	// Fixme: lots of code duplicated with the regular read. 
	// We should be able to just call ReadFrame repeatedly.
	// Actually this could even be done from the outside with MoveNext() -> push to cache -> repeat.
	
	int					iReadFrameResult;
	AVFrame				*pDecodingFrameBuffer; 
	AVPacket			packet;
	int					frameFinished;
	int					iSizeBuffer;
	int					iResult = 0;
	int					iFramesDecoded = 0;
	int64_t				iOldStart = 0;
	int64_t				iOldEnd = 0;
	bool				bCanceled = false;
	

	//---------------------------------------------------
	// Check what we need to load.
	// If reducing, reduces the selection.
	//---------------------------------------------------
	ImportStrategy strategy = PrepareSelection(_iStartTimeStamp, _iEndTimeStamp, _bForceReload);
	
	// Reinitialize the reading type in case we fail.
	m_PrimarySelection->iAnalysisMode = 0;
	
	// If not complete, we'll only decode frames we don't already have.
	if(strategy != ImportStrategy::Complete)
	{
		if(m_FrameList->Count > 0)
		{
			iOldStart = m_FrameList[0]->iTimeStamp;
			iOldEnd = m_FrameList[m_FrameList->Count - 1]->iTimeStamp;
			log->Debug(String::Format("Optimized sentinels: [{0}]->[{1}]", _iStartTimeStamp, _iEndTimeStamp));
		}
	}
	
	if(strategy != ImportStrategy::Reduction)
	{
		int  iEstimatedNumberOfFrames = EstimateNumberOfFrames(_iStartTimeStamp, _iEndTimeStamp);

		//-----------------------------------------
		// Seek au début de la selection (ou avant)
		// TODO : et si ret = -1 ?
		//-----------------------------------------
		int ret = av_seek_frame(m_pFormatCtx, m_iVideoStream, _iStartTimeStamp, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

		//-----------------------------------------------------------------------------------
		// Allocate video frames, one for decoding, one to hold the picture after conversion.
		//-----------------------------------------------------------------------------------
		pDecodingFrameBuffer = avcodec_alloc_frame();
		if(m_pCurrentDecodedFrameBGR != nullptr)
		{
			av_free(m_pCurrentDecodedFrameBGR);
		}
		m_pCurrentDecodedFrameBGR = avcodec_alloc_frame();
		
		if( (m_pCurrentDecodedFrameBGR != NULL) && (pDecodingFrameBuffer != NULL) )
		{
			// Final container (customized image size)
			iSizeBuffer = avpicture_get_size(m_PixelFormatFFmpeg, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
			if(iSizeBuffer > 0)
			{
				if(m_Buffer == nullptr)
					m_Buffer = new uint8_t[iSizeBuffer];

				// Assign appropriate parts of buffer to image planes in pFrameBGR
				avpicture_fill((AVPicture *)m_pCurrentDecodedFrameBGR, m_Buffer , m_PixelFormatFFmpeg, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);

				
				bool done = false;
				bool bFirstPass = true;

				//-----------------
				// Read the frames.
				//-----------------
				do
				{
					iReadFrameResult = av_read_frame( m_pFormatCtx, &packet);

					if(iReadFrameResult >= 0)
					{
						// Is this a packet from the video stream?
						if(packet.stream_index == m_iVideoStream)
						{
							// Decode video frame
							avcodec_decode_video2(m_pCodecCtx, pDecodingFrameBuffer, &frameFinished, &packet);

							// L'image est elle complète ? (En cas de B-frame ?)
							if(frameFinished)
							{
								// Update positions.
								SetTimestampFromPacket(packet.dts, packet.pts, true);

								if(bFirstPass && m_PrimarySelection->iCurrentTimeStamp > _iStartTimeStamp && _iStartTimeStamp >= 0)
								{
									// Fixme: duplicated code from ReadFrame.

									// If the current ts is already after the target, we are dealing with this kind of files
									// where the seek doesn't work as advertised. We'll seek back again, further before the target 
									// and then decode until we get to it.
									
									// Do this only once.
									bFirstPass = false;
									
									// place the new target well before the original one.
									// For some files, one additional second back is not enough. The seek is wrong by up to 4 seconds.
									// We also allow the target to go before 0.

									int iSecondsBack = 4;
									int64_t iForceSeekTimestamp = _iStartTimeStamp - ((int64_t)m_InfosVideo->fAverageTimeStampsPerSeconds * iSecondsBack);
									int64_t iMinTarget = System::Math::Min(iForceSeekTimestamp, (int64_t)0);
									
									// Do the seek.
									log->Error(String::Format("[Seek] - First decoded frame [{0}] already after start sentinel. Force seek {1} more seconds back to [{2}]", 
										m_PrimarySelection->iCurrentTimeStamp, iSecondsBack, iForceSeekTimestamp));
									
									//av_seek_frame(m_pFormatCtx, m_iVideoStream, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD);
									avformat_seek_file(m_pFormatCtx, m_iVideoStream, iMinTarget , iForceSeekTimestamp, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD); 
									avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

									// Free the packet that was allocated by av_read_frame
									av_free_packet(&packet);

									// Loop back to restart decoding frames until we get to the target.
									continue;
								}

								bFirstPass = false;

								// Attention, comme on a fait un seek, il est possible qu'on soit en train de décoder des images
								// situées AVANT le début de la selection. Tant que c'est le cas, on décode dans le vide.

								if( m_PrimarySelection->iCurrentTimeStamp >= _iStartTimeStamp)
								{
									iFramesDecoded++;

									if((_iEndTimeStamp > 0) && (m_PrimarySelection->iCurrentTimeStamp >= _iEndTimeStamp))
									{
										done = true;
									}
									
									RescaleAndConvert(m_pCurrentDecodedFrameBGR, pDecodingFrameBuffer, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, m_PixelFormatFFmpeg, m_InfosVideo->bDeinterlaced); 
									
									try
									{
										//------------------------------------------------------------------------------------
										// Accepte uniquement un Stride multiple de 4.
										// Tous les fichiers aux formats non standard sont refusés par le constructeur Bitmap.
										//------------------------------------------------------------------------------------
										IntPtr* ptr = new IntPtr((void*)m_pCurrentDecodedFrameBGR->data[0]); 
										int iImageStride	= m_pCurrentDecodedFrameBGR->linesize[0];
										Bitmap^ m_BmpImage = gcnew Bitmap( m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, iImageStride, this->DecodingPixelFormat, *ptr );
										
										//-------------------------------------------------------------------------------------------------------
										// Dupliquer complètement, 
										// sinon toutes les images vont finir par utiliser le même pointeur : m_pCurrentDecodedFrameBGR->data[0].
										// Bitmap.Clone n'est pas suffisant, on continue de pointer vers les mêmes données.
										//------------------------------------------------------------------------------------------------------- 
										DecompressedFrame^ DecFrame = gcnew DecompressedFrame();
										DecFrame->BmpImage = AForge::Imaging::Image::Clone(m_BmpImage, m_BmpImage->PixelFormat); 
										DecFrame->iTimeStamp = m_PrimarySelection->iCurrentTimeStamp;
										
										// testing
										//DecFrame->Hbmp = DecFrame->BmpImage->GetHbitmap();

										//---------------------------------------------------------------------------
										// En modes d'agrandissement, 
										// faire attention au chevauchement de la selection demandée avec l'existant.
										//---------------------------------------------------------------------------
										if((strategy == ImportStrategy::InsertionBefore) && (m_PrimarySelection->iCurrentTimeStamp < iOldStart))
										{
											log->Debug(String::Format("Inserting frame before the original selection - [{0}]", m_PrimarySelection->iCurrentTimeStamp));	
											m_FrameList->Insert(iFramesDecoded - 1, DecFrame);
										}
										else if((strategy == ImportStrategy::InsertionAfter) && (m_PrimarySelection->iCurrentTimeStamp > iOldEnd))
										{
											log->Debug(String::Format("Inserting frame after the original selection - [{0}]", m_PrimarySelection->iCurrentTimeStamp));
											m_FrameList->Add(DecFrame);
										}
										else if(strategy == ImportStrategy::Complete)
										{
											//log->Debug(String::Format("Appending frame to selection - [{0}]", m_PrimarySelection->iCurrentTimeStamp));
											m_FrameList->Add(DecFrame);
										}
										else
										{
											// We already have this one. Do nothing.
											log->Error(String::Format("Frame not imported. Already in selection - [{0}]", m_PrimarySelection->iCurrentTimeStamp ));
										}
										
										delete m_BmpImage;

										// Avoid crashing if there's a refresh of the main screen.
										m_BmpImage = DecFrame->BmpImage;
										
										// Report Progress
										if(m_bgWorker != nullptr)
										{
											// Check for cancellation.
											if(m_bgWorker->CancellationPending)
											{
												bCanceled = true;
												done = true;
											}
											else
											{
												m_bgWorker->ReportProgress(iFramesDecoded, iEstimatedNumberOfFrames);
											}
										}
									}
									catch(Exception^)
									{
										// TODO sortir en erreur desuite.
										done = true;
										log->Error("Conversion error during selection import.");
									}
								}
								else
								{
									log->Debug(String::Format("Decoded frame is before the new start sentinel - [{0}]", m_PrimarySelection->iCurrentTimeStamp));
								}
							}
							else
							{
								// Buffering frame. libav just read an I or P frame that will be presented later.
								// (But which was necessary to get now to decode a coming B frame.)
								SetTimestampFromPacket(packet.dts, packet.pts, false);
							}
						}
						else if(packet.stream_index == m_iAudioStream)
						{
							
							// Audio experimental code.
							// done: extract audio frames to a single buffer for the whole selection.
							// todo: convert to usable format.
							// todo: play sound.

							/*

							//-----------------------------------
							// Decode audio frame(s)
							// The packet can contain multiples audio frames (samples).
							// We buffer everything into pAudioBuffer and then copy this 
							// to our own buffer for the whole selection.
							//-----------------------------------
							
							// Next section inspired by ffmpeg-sharp code.

							// Prepare buffer. It will contain up to 1 second of samples.
							uint8_t* pAudioBuffer = nullptr;
							int iAudioBufferSize = FFMAX(packet.size * sizeof(*pAudioBuffer), AVCODEC_MAX_AUDIO_FRAME_SIZE);
							pAudioBuffer = (uint8_t*)av_malloc(iAudioBufferSize);
							log->Debug(String::Format("audio samples, allocated: {0}.", iAudioBufferSize));

							int iPacketSize = packet.size;
							uint8_t* pPacketData = (uint8_t*)packet.data;
							int iTotalOutput = 0;

							// May loop multiple times if more than one frame in the packet.
							do
							{
								//-------------------------------------------------------------------------------------
								// pAudioBuffer : the entire output buffer (will contain data from all samples in the packet).
								// iAudioBufferSize : size of the entire output buffer.
								// pcmWritePtr : pointer to the current position in the output buffer.
								// iTotalOutput : number of bytes read until now, for all samples in the packet.
								//
								// pPacketData : current position in the packet.
								// iPacketSize : remaining number of bytes to read from the packet.
								//
								// iOutputBufferUsedSize : number of bytes the current sample represents once decoded.
								// iUsedInputBytes : number of bytes read from the packet to decode the current sample.
								//-------------------------------------------------------------------------------------
							
								int16_t* pcmWritePtr = (int16_t*)(pAudioBuffer + iTotalOutput);
								int iOutputBufferUsedSize = iAudioBufferSize - iTotalOutput; 
								
								log->Debug(String::Format("Decoding part of an audio packet. iOutputBufferUsedSize:{0}, iTotalOutput:{1}", iOutputBufferUsedSize, iTotalOutput));

								// Decode.
								int iUsedInputBytes = avcodec_decode_audio2(m_pAudioCodecCtx, pcmWritePtr, &iOutputBufferUsedSize, pPacketData, iPacketSize);

								log->Debug(String::Format("Decoding part of an audio packet. iUsedInputBytes:{0}", iUsedInputBytes));

								if (iUsedInputBytes < 0)
								{
									log->Debug("Audio decoding error. Ignoring packet");
									break;
								}

								if (iOutputBufferUsedSize > 0)
								{
									iTotalOutput += iOutputBufferUsedSize;
								}

								pPacketData += iUsedInputBytes;
								iPacketSize -= iUsedInputBytes;
							}
							while (iPacketSize > 0);

							log->Debug(String::Format("iTotalOutput : {0}.", iTotalOutput));

							// Convert packet to usable format.
							//todo

							// Commit these samples to main buffer.
							log->Debug(String::Format("Commiting to main buffer m_AudioBufferUsedSize : {0}.", m_AudioBufferUsedSize));
							for(int i = 0;i<iTotalOutput;i++)
							{
								m_AudioBuffer[m_AudioBufferUsedSize + i] = pAudioBuffer[i];
							}
							m_AudioBufferUsedSize += iTotalOutput;

							// Cleaning
							av_free(pAudioBuffer);

							*/
						}
						
						// Free the packet that was allocated by av_read_frame
						av_free_packet(&packet);
					}
					else
					{
						// Terminaison par fin du parcours de l'ensemble de la vidéo, ou par erreur...
						done = true;
					}
				}
				while(!done);
			
				av_free(pDecodingFrameBuffer);
			}
			else
			{
				// Codec was opened succefully but we got garbage in width and height. (Some FLV files.)
				iResult = 2;	// SHOW_NEXT_FRAME_ALLOC_ERROR
			}
		}
		else
		{
			iResult = 2;	// SHOW_NEXT_FRAME_ALLOC_ERROR
		}


	}
	// If reduction, frames were deleted at PrepareSelection time.
	

	//----------------------------
	// Fin de l'import.
	//----------------------------
	if(m_FrameList->Count > 0)
	{
		if(bCanceled)
		{
			log->Debug("Extraction to memory was cancelled, discarding PrimarySelection.");
			m_PrimarySelection->iAnalysisMode = 0;
			m_PrimarySelection->iDurationFrame = 0;
			m_PrimarySelection->iCurrentFrame = -1;
			DeleteFrameList();

			// We don't destroy m_BmpImage here, because it might already have been filled back in another thread.
			// (When we cancel the bgWorker the program control was immediately returned.)
		}
		else
		{
			m_PrimarySelection->iAnalysisMode = 1;
			m_PrimarySelection->iDurationFrame = m_FrameList->Count;
			if(m_PrimarySelection->iCurrentFrame > m_PrimarySelection->iDurationFrame-1)
			{
				m_PrimarySelection->iCurrentFrame = m_PrimarySelection->iDurationFrame - 1;
			}
			else if(m_PrimarySelection->iCurrentFrame < 0)
			{
				m_PrimarySelection->iCurrentFrame = 0;
			}

			// Image en cours
			m_BmpImage = m_FrameList[m_PrimarySelection->iCurrentFrame]->BmpImage;
			

			// Test save audio in raw file.
			/*IntPtr ptr((void*)m_AudioBuffer);
			array<Byte>^ bytes = gcnew array<Byte>(m_AudioBufferUsedSize);
			Marshal::Copy(ptr, bytes, 0, m_AudioBufferUsedSize);
			File::WriteAllBytes("testrawaudio.raw", bytes);*/
		}
	}
	else
	{
		log->Error("Extraction to memory failed, discarding PrimarySelection.");
		m_PrimarySelection->iAnalysisMode = 0;
		m_PrimarySelection->iDurationFrame = 0;
		m_PrimarySelection->iCurrentFrame = -1;
		DeleteFrameList();
		// /!\ m_pCurrentDecodedFrameBGR has been invalidated.
		m_BmpImage = nullptr;
	}

}

// OBSOLETE
int64_t VideoReaderFFMpeg::GetFrameNumber(int64_t _iPosition)
{
	// Frame Number from TimeStamp.
	int iFrame = 0;
	if(m_FrameList != nullptr)
	{
		if(m_FrameList->Count > 0)
		{
			int64_t iCurrentTimeStamp = m_FrameList[0]->iTimeStamp;		
			while((iCurrentTimeStamp < _iPosition) && (iFrame < m_FrameList->Count-1))
			{
				iFrame++;
				iCurrentTimeStamp = m_FrameList[iFrame]->iTimeStamp;
			}
		}
	}

	return iFrame;
}
int VideoReaderFFMpeg::GetFirstStreamIndex(AVFormatContext* _pFormatCtx, int _iCodecType)
{
	//-----------------------------------------------
	// Look for the first stream of type _iCodecType.
	// return its index if found, -1 otherwise.
	//-----------------------------------------------

	unsigned int	iCurrentStreamIndex		= 0;
	unsigned int	iBestStreamIndex		= -1;
	int64_t			iBestFrames				= -1;

	// We loop around all streams and keep the one with most frames.
	while( iCurrentStreamIndex < _pFormatCtx->nb_streams)
	{
		if(_pFormatCtx->streams[iCurrentStreamIndex]->codec->codec_type == _iCodecType)
		{
			int64_t frames = _pFormatCtx->streams[iCurrentStreamIndex]->nb_frames;
			if(frames > iBestFrames)
			{
				iBestFrames = frames;
				iBestStreamIndex = iCurrentStreamIndex;
			}
		}
		iCurrentStreamIndex++;
	}

	return (int)iBestStreamIndex;
}

ImportStrategy VideoReaderFFMpeg::PrepareSelection(int64_t% _iStartTimeStamp, int64_t% _iEndTimeStamp, bool _bForceReload)
{
	//--------------------------------------------------------------
	// détermine si la selection à réellement besoin d'être chargée.
	// Modifie simplement la selection en cas de réduction.
	// Spécifie où et quelles frames doivent être chargées sinon.
	//--------------------------------------------------------------

	ImportStrategy result;

	if(m_PrimarySelection->iAnalysisMode == 0 || _bForceReload)
	{
		//----------------------------------------------------------------------------		
		// On était pas en mode Analyse ou forcé : Chargement complet.
		// (Garder la liste même quand on sort du mode analyse, pour réutilisation ? )
		//----------------------------------------------------------------------------
		DeleteFrameList();

		log->Debug(String::Format("Preparing Selection for import : [{0}]->[{1}].", _iStartTimeStamp, _iEndTimeStamp));
		m_FrameList = gcnew List<DecompressedFrame^>();

		result = ImportStrategy::Complete; 
	}
	else
	{
		// Traitement différent selon les frames déjà chargées...
		
		if(m_FrameList == nullptr)
		{
			// Ne devrait pas passer par là.
			m_FrameList = gcnew List<DecompressedFrame^>();
			result = ImportStrategy::Complete; 
		}
		else
		{
			int64_t iOldStart = m_FrameList[0]->iTimeStamp;
			int64_t iOldEnd = m_FrameList[m_FrameList->Count - 1]->iTimeStamp;

			log->Debug(String::Format("Preparing Selection for import. Current selection: [{0}]->[{1}], new selection: [{2}]->[{3}].", iOldStart, iOldEnd, _iStartTimeStamp, _iEndTimeStamp));

			// Since some videos are causing problems in timestamps reported, it is possible that we end up with double updates.
			// e.g. reduction at left AND expansion at right, expansion on both sides, etc.
			// We'll deal with reduction first, then expansions.

			if(_iEndTimeStamp < iOldEnd)
			{
				log->Debug("Frames needs to be deleted at the end of the existing selection.");					
				int iNewLastIndex = (int)GetFrameNumber(_iEndTimeStamp);
				
				for(int i=iNewLastIndex+1;i<m_FrameList->Count;i++)
				{
					delete m_FrameList[i]->BmpImage;
				}
				m_FrameList->RemoveRange(iNewLastIndex+1, (m_FrameList->Count-1) - iNewLastIndex);

				// Reduced until further notice.
				result = ImportStrategy::Reduction;
			}

			if(_iStartTimeStamp > iOldStart)
			{
				log->Debug("Frames needs to be deleted at the begining of the existing selection.");
				int iNewFirstIndex = (int)GetFrameNumber(_iStartTimeStamp);
				
				for(int i=0;i<iNewFirstIndex;i++)
				{
					delete m_FrameList[i]->BmpImage;
				}

				m_FrameList->RemoveRange(0, iNewFirstIndex);
				
				// Reduced until further notice.
				result = ImportStrategy::Reduction;
			}


			// We expand the selection if the new sentinel is at least one frame out the current selection.
			// Expanding on both sides is not supported yet.

			if(_iEndTimeStamp >= iOldEnd + m_InfosVideo->iAverageTimeStampsPerFrame)
			{
				log->Debug("Frames needs to be added at the end of the existing selection.");
				_iStartTimeStamp = iOldEnd;
				result = ImportStrategy::InsertionAfter;
			}
			else if(_iStartTimeStamp <= iOldStart - m_InfosVideo->iAverageTimeStampsPerFrame)
			{
				log->Debug("Frames needs to be added at the begining of the existing selection.");
				_iEndTimeStamp = iOldStart;
				result = ImportStrategy::InsertionBefore;
			}
		}
	}

	return result;
}
void VideoReaderFFMpeg::DeleteFrameList()
{
	if(m_FrameList != nullptr)
	{
		for(int i = 0;i<m_FrameList->Count;i++) 
		{ 
			delete m_FrameList[i]->BmpImage;
			delete m_FrameList[i]; 
		}			
		delete m_FrameList;
	}
}

int VideoReaderFFMpeg::EstimateNumberOfFrames( int64_t _iStartTimeStamp, int64_t _iEndTimeStamp) 
{
	//-------------------------------------------------------------
	// Calcul du nombre d'images à charger (pour le ReportProgress)
	//-------------------------------------------------------------
	int iEstimatedNumberOfFrames = 0;
	int64_t iSelectionDurationTimeStamps;
	if(_iEndTimeStamp == -1) 
	{ 
		iSelectionDurationTimeStamps = m_InfosVideo->iDurationTimeStamps - _iStartTimeStamp; 
	}
	else 
	{ 
		iSelectionDurationTimeStamps = _iEndTimeStamp - _iStartTimeStamp; 
	}

	iEstimatedNumberOfFrames = (int)(iSelectionDurationTimeStamps / m_InfosVideo->iAverageTimeStampsPerFrame);

	return iEstimatedNumberOfFrames;
}
bool VideoReaderFFMpeg::RescaleAndConvert(AVFrame* _pOutputFrame, AVFrame* _pInputFrame, int _OutputWidth, int _OutputHeight, int _OutputFmt, bool _bDeinterlace)
{
	//------------------------------------------------------------------------
	// Function used by GetNextFrame, ImportAnalysis and SaveMovie.
	// Take the frame we just decoded and turn it to the right size/deint/fmt.
	// todo: sws_getContext could be done only once.
	//------------------------------------------------------------------------
	bool bSuccess = true;
	SwsContext* pSWSCtx = sws_getContext(
		m_pCodecCtx->width, 
		m_pCodecCtx->height, 
		m_pCodecCtx->pix_fmt, 
		_OutputWidth, 
		_OutputHeight, 
		(PixelFormat)_OutputFmt, 
		DecodingQuality, 
		nullptr, nullptr, nullptr); 
		
	uint8_t** ppOutputData = nullptr;
	int* piStride = nullptr;
	uint8_t* pDeinterlaceBuffer = nullptr;

	if(_bDeinterlace)
	{
		AVPicture*	pDeinterlacingFrame;
		AVPicture	tmpPicture;

		// Deinterlacing happens before resizing.
		int iSizeDeinterlaced = avpicture_get_size(m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);
		
		pDeinterlaceBuffer = new uint8_t[iSizeDeinterlaced];
		pDeinterlacingFrame = &tmpPicture;
		avpicture_fill(pDeinterlacingFrame, pDeinterlaceBuffer, m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);

		int resDeint = avpicture_deinterlace(pDeinterlacingFrame, (AVPicture*)_pInputFrame, m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);

		if(resDeint < 0)
		{
			// Deinterlacing failed, use original image.
			log->Debug("Deinterlacing failed, use original image.");
			//sws_scale(pSWSCtx, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
			ppOutputData = _pInputFrame->data;
			piStride = _pInputFrame->linesize;
		}
		else
		{
			// Use deinterlaced image.
			//sws_scale(pSWSCtx, pDeinterlacingFrame->data, pDeinterlacingFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
			ppOutputData = pDeinterlacingFrame->data;
			piStride = pDeinterlacingFrame->linesize;
		}
	}
	else
	{
		//sws_scale(pSWSCtx, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
		ppOutputData = _pInputFrame->data;
		piStride = _pInputFrame->linesize;
	}

	try
	{
		sws_scale(pSWSCtx, ppOutputData, piStride, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
	}
	catch(Exception^)
	{
		bSuccess = false;
		log->Error("RescaleAndConvert Error : sws_scale failed.");
	}

	// Clean Up.
	sws_freeContext(pSWSCtx);
	
	if(pDeinterlaceBuffer != nullptr)
	{
		delete []pDeinterlaceBuffer;
		pDeinterlaceBuffer = nullptr;
	}

	return bSuccess;

}
void VideoReaderFFMpeg::SetDecodingSize(Kinovea::Video::ImageAspectRatio _ratio)
{
	// Set the image geometry according to the pixel aspect ratio choosen.
	log->DebugFormat("Image aspect ratio: {0}", _ratio);
	
	// Image height from aspect ratio. (width never moves)
	switch(_ratio)
	{
	case Kinovea::Video::ImageAspectRatio::Force43:
		m_VideoInfo.DecodingSize.Height = (int)((m_VideoInfo.OriginalSize.Width * 3.0) / 4.0);
		break;
	case Kinovea::Video::ImageAspectRatio::Force169:
		m_VideoInfo.DecodingSize.Height = (int)((m_VideoInfo.OriginalSize.Width * 9.0) / 16.0);
		break;
	case Kinovea::Video::ImageAspectRatio::ForcedSquarePixels:
		m_VideoInfo.DecodingSize.Height = m_VideoInfo.OriginalSize.Height;
		break;
	case Kinovea::Video::ImageAspectRatio::Auto:
	default:
		m_VideoInfo.DecodingSize.Height = (int)((double)m_VideoInfo.OriginalSize.Height / m_VideoInfo.PixelAspectRatio);
		break;
	}

	// Fix unsupported width for conversion to .NET Bitmap.
	if(m_VideoInfo.DecodingSize.Width % 4 != 0)
		m_VideoInfo.DecodingSize.Width = 4 * ((m_VideoInfo.OriginalSize.Width / 4) + 1);
	else
		m_VideoInfo.DecodingSize.Width = m_VideoInfo.OriginalSize.Width;

	log->DebugFormat("Image size: Original:{0}, Decoding:{1}", m_VideoInfo.OriginalSize, m_VideoInfo.DecodingSize);
}
void VideoReaderFFMpeg::DumpInfo()
{
	log->Debug("---------------------------------------------------");
	log->Debug("[File] - Filename : " + Path::GetFileName(m_VideoInfo.FilePath));
	log->DebugFormat("[Container] - Name: {0} ({1})", gcnew String(m_pFormatCtx->iformat->name), gcnew String(m_pFormatCtx->iformat->long_name));
	DumpStreamsInfos(m_pFormatCtx);
	log->Debug("[Container] - Duration (s): " + (double)m_pFormatCtx->duration/1000000);
	log->Debug("[Container] - Bit rate: " + m_pFormatCtx->bit_rate);
	if(m_pFormatCtx->streams[m_iVideoStream]->nb_frames > 0)
		log->DebugFormat("[Stream] - Duration (frames): {0}", m_pFormatCtx->streams[m_iVideoStream]->nb_frames);
	else
		log->Debug("[Stream] - Duration (frames): Unavailable.");
	log->DebugFormat("[Stream] - PTS wrap bits: {0}", m_pFormatCtx->streams[m_iVideoStream]->pts_wrap_bits);
	log->DebugFormat("[Stream] - TimeBase: {0}:{1}", m_pFormatCtx->streams[m_iVideoStream]->time_base.den, m_pFormatCtx->streams[m_iVideoStream]->time_base.num);
	log->DebugFormat("[Stream] - Average timestamps per seconds: {0}", m_VideoInfo.AverageTimeStampsPerSeconds);
	log->DebugFormat("[Container] - Start time (µs): {0}", m_pFormatCtx->start_time);
	log->DebugFormat("[Container] - Start timestamp: {0}", m_VideoInfo.FirstTimeStamp);
	log->DebugFormat("[Codec] - Name: {0}, id:{1}", gcnew String(m_pCodecCtx->codec_name), (int)m_pCodecCtx->codec_id);
	log->DebugFormat("[Codec] - TimeBase: {0}:{1}", m_pCodecCtx->time_base.den, m_pCodecCtx->time_base.num);
	log->Debug("[Codec] - Bit rate: " + m_pCodecCtx->bit_rate);
	log->Debug("Duration in timestamps: " + m_VideoInfo.DurationTimeStamps);
	log->Debug("Duration in seconds (computed): " + (double)(double)m_VideoInfo.DurationTimeStamps/(double)m_VideoInfo.AverageTimeStampsPerSeconds);
	log->Debug("Average Fps: " + m_VideoInfo.FramesPerSeconds);
	log->Debug("Average Frame Interval (ms): " + m_VideoInfo.FrameIntervalMilliseconds);
	log->Debug("Average Timestamps per frame: " + m_VideoInfo.AverageTimeStampsPerFrame);
	log->DebugFormat("[Codec] - Has B Frames: {0}", m_pCodecCtx->has_b_frames);
	log->Debug("[Codec] - Width (pixels): " + m_pCodecCtx->width);
	log->Debug("[Codec] - Height (pixels): " + m_pCodecCtx->height);
	log->Debug("[Codec] - Pixel Aspect Ratio: " + m_VideoInfo.PixelAspectRatio);
	log->Debug("---------------------------------------------------");
}


void VideoReaderFFMpeg::DumpStreamsInfos(AVFormatContext* _pFormatCtx)
{
	log->Debug("[Container] - Number of streams: " + _pFormatCtx->nb_streams);

	for(int i = 0;i<(int)_pFormatCtx->nb_streams;i++)
	{
		String^ streamType;
		
		switch((int)_pFormatCtx->streams[i]->codec->codec_type)
		{
		case AVMEDIA_TYPE_VIDEO:
			streamType = "AVMEDIA_TYPE_VIDEO";
			break;
		case AVMEDIA_TYPE_AUDIO:
			streamType = "AVMEDIA_TYPE_AUDIO";
			break;
		case AVMEDIA_TYPE_DATA:
			streamType = "AVMEDIA_TYPE_DATA";
			break;
		case AVMEDIA_TYPE_SUBTITLE:
			streamType = "AVMEDIA_TYPE_SUBTITLE";
			break;
		case AVMEDIA_TYPE_UNKNOWN:
		default:
			streamType = "AVMEDIA_TYPE_UNKNOWN";
			break;
		}

		log->Debug(String::Format("[Stream] #{0}, Type : {1}, {2}", i, streamType, _pFormatCtx->streams[i]->nb_frames));
	}
}
void VideoReaderFFMpeg::DumpFrameType(int _type)
{
	switch(_type)
	{
	case FF_I_TYPE:
		log->Debug("(I) Frame +++++");
		break;
	case FF_P_TYPE:
		log->Debug("(P) Frame --");
		break;
	case FF_B_TYPE:
		log->Debug("(B) Frame .");
		break;
	case FF_S_TYPE:
		log->Debug("Frame : S(GMC)-VOP MPEG4");
		break;
	case FF_SI_TYPE:
		log->Debug("Switching Intra");
		break;
	case FF_SP_TYPE:
		log->Debug("Switching Predicted");
		break;
	case FF_BI_TYPE:
		log->Debug("FF_BI_TYPE");
		break;
	}
}