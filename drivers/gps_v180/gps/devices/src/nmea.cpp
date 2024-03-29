/****************************************************************************
 *
 *   Copyright (c) 2012-2016 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/* @file nmea.cpp
*/

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <ctime>

#include "nmea.h"
#include <systemlib/mavlink_log.h>

extern orb_advert_t mavlink_log_pub;

GPSDriverNMEA::GPSDriverNMEA(GPSCallbackPtr callback, void *callback_user,
				   struct vehicle_gps_position_s *gps_position,
				   struct satellite_info_s *satellite_info,
				   uint32_t nmea_baud) :
	GPSHelper(callback, callback_user),
	_satellite_info(satellite_info),
	_gps_position(gps_position),
	_baudrate(nmea_baud)
{
	decodeInit();
	_decode_state = NMEA_DECODE_UNINIT;
	_rx_buffer_bytes = 0;
}

/*
 * All NMEA descriptions are taken from
 * http://www.trimble.com/OEM_ReceiverHelp/V4.44/en/NMEA-0183messages_MessageOverview.html
 */

int GPSDriverNMEA::handleMessage(int len)
{
	char *endp;

	if (len < 7) {
		return 0;
	}

	int uiCalcComma = 0;

	for (int i = 0 ; i < len; i++) {
		if (_rx_buffer[i] == ',') { uiCalcComma++; }
	}

	char *bufptr = (char *)(_rx_buffer + 6);
	int ret = 0;

    	if ((memcmp(_rx_buffer + 3, "ZDA,", 3) == 0) && (uiCalcComma == 6)) {
		/*
		UTC day, month, and year, and local time zone offset
		An example of the ZDA message string is:

		$GPZDA,172809.456,12,07,1996,00,00*45

		ZDA message fields
		Field	Meaning
		0	Message ID $GPZDA
		1	UTC
		2	Day, ranging between 01 and 31
		3	Month, ranging between 01 and 12
		4	Year
		5	Local time zone offset from GMT, ranging from 00 through 13 hours
		6	Local time zone offset from GMT, ranging from 00 through 59 minutes
		7	The checksum data, always begins with *
		Fields 5 and 6 together yield the total offset. For example, if field 5 is -5 and field 6 is +15, local time is 5 hours and 15 minutes earlier than GMT.
		*/
		double nmea_time = 0.0;
		int day = 0, month = 0, year = 0, local_time_off_hour __attribute__((unused)) = 0,
		    local_time_off_min __attribute__((unused)) = 0;

		if (bufptr && *(++bufptr) != ',') { nmea_time = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { day = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { month = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { year = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { local_time_off_hour = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { local_time_off_min = strtol(bufptr, &endp, 10); bufptr = endp; }


		int nmea_hour = static_cast<int>(nmea_time / 10000);
		int nmea_minute = static_cast<int>((nmea_time - nmea_hour * 10000) / 100);
		double nmea_sec = static_cast<double>(nmea_time - nmea_hour * 10000 - nmea_minute * 100);

		/*
		 * convert to unix timestamp
		 */
		struct tm timeinfo = {};
		timeinfo.tm_year = year - 1900;
		timeinfo.tm_mon = month - 1;
		timeinfo.tm_mday = day;
		timeinfo.tm_hour = nmea_hour;
		timeinfo.tm_min = nmea_minute;
		timeinfo.tm_sec = int(nmea_sec);
		timeinfo.tm_isdst = 0;

#ifndef NO_MKTIME
		time_t epoch = mktime(&timeinfo);

		if (epoch > GPS_EPOCH_SECS) {
			uint64_t usecs = static_cast<uint64_t>((nmea_sec - static_cast<uint64_t>(nmea_sec))) * 1000000;

			// FMUv2+ boards have a hardware RTC, but GPS helps us to configure it
			// and control its drift. Since we rely on the HRT for our monotonic
			// clock, updating it from time to time is safe.

			timespec ts{};
			ts.tv_sec = epoch;
			ts.tv_nsec = usecs * 1000;

			setClock(ts);

			_gps_position->time_utc_usec = static_cast<uint64_t>(epoch) * 1000000ULL;
			_gps_position->time_utc_usec += usecs;

		} else {
			_gps_position->time_utc_usec = 0;
		}

#else
		_gps_position->time_utc_usec = 0;
#endif

		_last_timestamp_time = gps_absolute_time();

	}	else if ((memcmp(_rx_buffer + 3, "GGA,", 3) == 0) && (uiCalcComma == 14)) {
		/*
		  Time, position, and fix related data
		  An example of the GBS message string is:
		  $xxGGA,time,lat,NS,long,EW,quality,numSV,HDOP,alt,M,sep,M,diffAge,diffStation*cs<CR><LF>

		  $GPGGA,172814.0,3723.46587704,N,12202.26957864,W,2,6,1.2,18.893,M,-25.669,M,2.0,0031*4F

		  Note - The data string exceeds the nmea standard length.
		  GGA message fields
		  Field   Meaning
		  0   Message ID $GPGGA
		  1   UTC of position fix
		  2   Latitude
		  3   Direction of latitude:
		  N: North
		  S: South
		  4   Longitude
		  5   Direction of longitude:
		  E: East
		  W: West
		  6   GPS Quality indicator:
		  0: Fix not valid
		  1: GPS fix
		  2: Differential GPS fix, OmniSTAR VBS
		  4: Real-Time Kinematic, fixed integers
		  5: Real-Time Kinematic, float integers, OmniSTAR XP/HP or Location RTK
		  7   Number of SVs in use, range from 00 through to 24+
		  8   HDOP
		  9   Orthometric height (MSL reference)
		  10  M: unit of measure for orthometric height is meters
		  11  Geoid separation
		  12  M: geoid separation measured in meters
		  13  Age of differential GPS data record, Type 1 or Type 9. Null field when DGPS is not used.
		  14  Reference station ID, range 0000-4095. A null field when any reference station ID is selected and no corrections are received1.
		  15
		  The checksum data, always begins with *
		*/
		double nmea_time __attribute__((unused)) = 0.0, lat = 0.0, lon = 0.0, alt = 0.0;
		float hdop = 99.9;
		int  num_of_sv __attribute__((unused)) = 0, fix_quality  = 0;
		char ns = '?', ew = '?';


		if (bufptr && *(++bufptr) != ',') { nmea_time = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { lat = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { ns = *(bufptr++); }

		if (bufptr && *(++bufptr) != ',') { lon = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { ew = *(bufptr++); }

		if (bufptr && *(++bufptr) != ',') { fix_quality = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { num_of_sv = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { hdop = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { alt = strtod(bufptr, &endp); bufptr = endp; }

		if (ns == 'S') {
			lat = -lat;
		}

		if (ew == 'W') {
			lon = -lon;
		}

		/* convert from degrees, minutes and seconds to degrees * 1e7 */
		_gps_position->lon = static_cast<int>((int(lon * 0.01) + (lon * 0.01 - int(lon * 0.01)) * 100.0 / 60.0) * 10000000);
		_gps_position->lat = static_cast<int>((int(lat * 0.01) + (lat * 0.01 - int(lat * 0.01)) * 100.0 / 60.0) * 10000000);
		_gps_position->hdop = static_cast<float>(hdop);
		_gps_position->alt = static_cast<int>(alt * 1000);
		_rate_count_lat_lon++;

		if (fix_quality <= 0) {
			_gps_position->fix_type = 0;

		} else {
			/*
			 * in this NMEA message float integers (value 5) mode has higher value than fixed integers (value 4), whereas it provides lower quality,
			 * and since value 3 is not being used, I "moved" value 5 to 3 to add it to _gps_position->fix_type
			 */
			if (fix_quality == 5) { fix_quality = 3; }

			/*
			 * fix quality 1 means just a normal 3D fix, so I'm subtracting 1 here. This way we'll have 3 for auto, 4 for DGPS, 5 for floats, 6 for fixed.
			 */
			_gps_position->fix_type = 3 + fix_quality - 1;
		}

		_gps_position->timestamp = gps_absolute_time();

		_gps_position->vel_ned_valid = true;                      /**< Flag to indicate if NED speed is valid */
		_gps_position->c_variance_rad = 0.1f;
		ret = 1;

//		mavlink_log_info(&mavlink_log_pub, "get GGA data ");

	// } else if ((memcmp(_rx_buffer + 3, "GNS,", 3) == 0 ) && (uiCalcComma == 13)) {
	} else if (memcmp(_rx_buffer + 3, "GNS,", 3) == 0 ) {
		/*
Message GNS
Description GNSS fix data
Firmware Supported on:
• u-blox 9 protocol versions 27 and 27.1
Type Output Message
Comment The output of this message is dependent on the currently selected datum (default:
WGS84)
Time and position, together with GNSS fixing related data (number of satellites in use, and
the resulting HDOP, age of differential data if in use, etc.).
ID for CFG-MSG Number of fields
Message Info 0xF0 0x0D 16
Message Structure:
$xxGNS,time,lat,NS,long,EW,posMode,numSV,HDOP,alt,altRef,diffAge,diffStation,navStatus*cs<CR><LF>
Example:
$GPGNS,091547.00,5114.50897,N,00012.28663,W,AA,10,0.83,111.1,45.6,,,V*71
FieldNo.  Name    Unit     Format                  Example Description
0        xxGNS    -       string            $GPGNS GNS Message ID (xx = current Talker ID)
1        time     -       hhmmss.ss         091547.00 UTC time, see note on UTC representation
2        lat      -       ddmm.mmmmm        5114.50897 Latitude (degrees & minutes), see format description
3        NS       -       character         N North/South indicator
4        long     -       dddmm.mmmmm       00012.28663 Longitude (degrees & minutes), see format description
5        EW       -       character         E East/West indicator
6      posMode    -       character         AA Positioning mode, see position fix flags description. First character for GPS, second character forGLONASS
7       numSV     -       numeric         10 Number of satellites used (range: 0-99)
8         HDOP    -       numeric         0.83 Horizontal Dilution of Precision
9         alt     m       numeric         111.1 Altitude above mean sea level
10        sep    m        numeric         45.6 Geoid separation: difference between ellipsoid and mean sea level UBX-18010854 - R05 Advance Information Page 18 of 262 u-blox ZED-F9P Interface Description - Manual GNS continued
11    diffAge    s        numeric         - Age of differential corrections (blank when DGPS is not used)
12 diffStation   -        numeric         - ID of station providing differential corrections (blank when DGPS is not used)
13 navStatus    -         character         V Navigational status indicator (V = Equipment is not providing navigational status information) NMEA v4.10 and above only
14 cs - hexadecimal *71 Checksum
15 <CR><LF> - character - Carriage return and line feed
		*/
		double nmea_time __attribute__((unused)) = 0.0; 
		double lat = 0.0, lon = 0.0;
		char pos_Mode __attribute__((unused)) =  'N';
		int num_of_sv =0;
		float alt =0.0;
		float HDOP =0;
		char ns = '?', ew = '?';
	
		if (bufptr && *(++bufptr) != ',') { nmea_time = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { lat = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { ns = *(bufptr++); }

		if (bufptr && *(++bufptr) != ',') { lon = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { ew = *(bufptr++);}

					do {
				pos_Mode +=  *(bufptr);
					} while (*(++bufptr) != ',');
		
		if (bufptr && *(++bufptr) != ',') { num_of_sv = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { HDOP = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { alt = strtod(bufptr, &endp); bufptr = endp; }
	
		if (ns == 'S') {
			lat = -lat;
		}

		if (ew == 'W') {
			lon = -lon;
		}
		/* convert from degrees, minutes and seconds to degrees * 1e7 */
		_gps_position->lat = static_cast<int>((int(lat * 0.01) + (lat * 0.01 - int(lat * 0.01)) * 100.0 / 60.0) * 10000000);
		_gps_position->lon = static_cast<int>((int(lon * 0.01) + (lon * 0.01 - int(lon * 0.01)) * 100.0 / 60.0) * 10000000);
		_gps_position->hdop = static_cast<float>(HDOP);
		_gps_position->alt = static_cast<int>(alt * 1000);

		_rate_count_lat_lon++;

		_gps_position->satellites_used = static_cast<int>(num_of_sv);

		// _gps_position->timestamp = gps_absolute_time();

		// ret = 1;

		// mavlink_log_info(&mavlink_log_pub, "get GNS posMode %c/nsv %.d/ hdop %.2f",

		// (char)(pos_Mode),
		// (int)(num_of_sv),
		// (double)(HDOP));

	}	else if ((memcmp(_rx_buffer + 3, "RMC,", 3) == 0 )) {
		/*
		  Position, velocity, and time
		  The RMC string is:

           $xxRMC,time,status,lat,NS,long,EW,spd,cog,date,mv,mvEW,posMode,navStatus*cs<CR><LF>
		  The Talker ID ($--) will vary depending on the satellite system used for the position solution:

		  GPRMC message fields
		  Field	Meaning
		   0	Message ID $GPRMC
		   1	UTC of position fix
		   2	Status A=active or V=void
		   3	Latitude
		   4	Longitude
		   5	Speed over the ground in knots
		   6	Track angle in degrees (True)
		   7	Date
		   8	Magnetic variation in degrees
		   9	The checksum data, always begins with *
		*/
		double nmea_time = 0.0; 
		char Sts __attribute__((unused));
		double lat = 0.0, lon = 0.0;
		float ground_speed_K =0.0;
		float A_track =0.0;
		int nmea_date =0 ;
		float Mag_var __attribute__((unused)) = 0.0;
		char ns = '?', ew = '?';

		if (bufptr && *(++bufptr) != ',') { nmea_time = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { Sts = *(bufptr++); }

		if (bufptr && *(++bufptr) != ',') { lat = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { ns = *(bufptr++); }

		if (bufptr && *(++bufptr) != ',') { lon = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { ew = *(bufptr++); }

		if (bufptr && *(++bufptr) != ',') { ground_speed_K = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { A_track = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { nmea_date = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { Mag_var = strtod(bufptr, &endp); bufptr = endp; }
		
		if (ns == 'S') {
			lat = -lat;
		}

		if (ew == 'W') {
			lon = -lon;
		}

        float track_rad = static_cast<float>(A_track) * M_PI_F/ 180.0f;
        float velocity_ms =static_cast<float>(ground_speed_K) / 1.9438445f;
        float velocity_north =static_cast<float>(velocity_ms) * cosf(track_rad);
        float velocity_east  = static_cast<float>(velocity_ms) *sinf(track_rad);
		int nmea_hour = static_cast<int>(nmea_time / 10000);
		int nmea_minute = static_cast<int>((nmea_time - nmea_hour * 10000) / 100);
		double nmea_sec = static_cast<double>(nmea_time - nmea_hour * 10000 - nmea_minute * 100);
		int nema_day= static_cast<int>(nmea_date / 10000);
		int nmea_mth = static_cast<int>((nmea_date - nema_day * 10000) / 100);
		int nmea_year = static_cast<int>(nmea_date - nema_day * 10000 - nmea_mth * 100);
		/* convert from degrees, minutes and seconds to degrees * 1e7 */
		_gps_position->lat = static_cast<int>((int(lat * 0.01) + (lat * 0.01 - int(lat * 0.01)) * 100.0 / 60.0) * 10000000);
		_gps_position->lon = static_cast<int>((int(lon * 0.01) + (lon * 0.01 - int(lon * 0.01)) * 100.0 / 60.0) * 10000000);

		_rate_count_lat_lon++;

        _gps_position->vel_m_s = velocity_ms;   
        _gps_position->vel_n_m_s =velocity_north;   
        _gps_position->vel_e_m_s =velocity_east;
        _gps_position->cog_rad =track_rad;
		_gps_position->vel_ned_valid = true; /**< Flag to indicate if NED speed is valid */
		_gps_position->c_variance_rad = 0.1f;
		_gps_position->s_variance_m_s = 0;
		_rate_count_vel++;

		/*
		 * convert to unix timestamp
		 */
		struct tm timeinfo = {};
		timeinfo.tm_year = nmea_year + 2000;
		timeinfo.tm_mon = nmea_mth;
		timeinfo.tm_mday = nema_day;
		timeinfo.tm_hour = nmea_hour;
		timeinfo.tm_min = nmea_minute;
		timeinfo.tm_sec = int(nmea_sec);
		timeinfo.tm_isdst = 0;

#ifndef NO_MKTIME
		time_t epoch = mktime(&timeinfo);

		if (epoch > GPS_EPOCH_SECS) {
			uint64_t usecs = static_cast<uint64_t>((nmea_sec - static_cast<uint64_t>(nmea_sec))) * 1000000;

			// FMUv2+ boards have a hardware RTC, but GPS helps us to configure it
			// and control its drift. Since we rely on the HRT for our monotonic
			// clock, updating it from time to time is safe.

			timespec ts{};
			ts.tv_sec = epoch;
			ts.tv_nsec = usecs * 1000;

			setClock(ts);

			_gps_position->time_utc_usec = static_cast<uint64_t>(epoch) * 1000000ULL;
			_gps_position->time_utc_usec += usecs;

		} else {
			_gps_position->time_utc_usec = 0;
		}

#else
		_gps_position->time_utc_usec = 0;
#endif
		_last_timestamp_time = gps_absolute_time();

		// mavlink_log_info(&mavlink_log_pub, "get RMC data ");

	}	else if ((memcmp(_rx_buffer + 3, "GST,", 3) == 0)) {
		/*
		  Position error statistics
		  An example of the GST message string is:

		  $GPGST,172814.0,0.006,0.023,0.020,273.6,0.023,0.020,0.031*6A

		  The Talker ID ($--) will vary depending on the satellite system used for the position solution:

		  $GP - GPS only
		  $GL - GLONASS only
		  $GN - Combined
		  GST message fields
		  Field   Meaning
		  0   Message ID $GPGST
		  1   UTC of position fix
		  2   RMS value of the pseudorange residuals; includes carrier phase residuals during periods of RTK (float) and RTK (fixed) processing
		  3   Error ellipse semi-major axis 1 sigma error, in meters
		  4   Error ellipse semi-minor axis 1 sigma error, in meters
		  5   Error ellipse orientation, degrees from true north
		  6   Latitude 1 sigma error, in meters
		  7   Longitude 1 sigma error, in meters
		  8   Height 1 sigma error, in meters
		  9   The checksum data, always begins with *
		*/
		float nmea_time __attribute__((unused)) = 0.0, lat_err = 0.0, lon_err = 0.0, alt_err = 0.0;
		float min_err __attribute__((unused)) = 0.0, maj_err __attribute__((unused)) = 0.0,
		deg_from_north __attribute__((unused)) = 0.0, rms_err __attribute__((unused)) = 0.0;

		if (bufptr && *(++bufptr) != ',') { nmea_time = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { rms_err = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { maj_err = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { min_err = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { deg_from_north = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { lat_err = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { lon_err = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { alt_err = strtod(bufptr, &endp); bufptr = endp; }

		_gps_position->eph = sqrtf(static_cast<float>(lat_err) * static_cast<float>(lat_err)
					   + static_cast<float>(lon_err) * static_cast<float>(lon_err));
		_gps_position->epv = static_cast<float>(alt_err);

		// mavlink_log_info(&mavlink_log_pub, "get GST data ");		

	} else if ((memcmp(_rx_buffer + 3, "GSA,", 3) == 0)) {
		/*
		  GPS DOP and active satellites
		  An example of the GSA message string is:
          $GPGSA,<1>,<2>,<3>,<3>,,,,,<3>,<3>,<3>,<4>,<5>,<6>*<7><CR><LF>

		  GSA message fields
           Field	Meaning
               0	Message ID $GPGSA
               1	Mode 1, M = manual, A = automatic
               2	Mode 2, Fix type, 1 = not available, 2 = 2D, 3 = 3D
               3	PRN number, 01 through 32 for GPS, 33 through 64 for SBAS, 64+ for GLONASS
               4 	PDOP: 0.5 through 99.9
               5	HDOP: 0.5 through 99.9
               6	VDOP: 0.5 through 99.9
               7	The checksum data, always begins with *
		*/
	    char M_pos __attribute__((unused)) = ' ';
		int fix_mode __attribute__((unused)) = 0;
		int sv_temp __attribute__((unused)) = 0;   
		float pdop  __attribute__((unused)) = 99.9, hdop = 99.9 ,vdop = 99.9;	
		if (bufptr && *(++bufptr) != ',') { M_pos = *(bufptr++); }

		if (bufptr && *(++bufptr) != ',') { fix_mode = strtod(bufptr, &endp); bufptr = endp; }

        for (int y = 0; y < 12; y++){
			if (bufptr && *(++bufptr )!= ',') {sv_temp = strtod(bufptr, &endp); bufptr = endp; }
		}
		if (bufptr && *(++bufptr) != ',') { pdop = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { hdop = strtod(bufptr, &endp); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { vdop = strtod(bufptr, &endp); bufptr = endp; }

		_gps_position->hdop = static_cast<float>(hdop);
		_gps_position->vdop = static_cast<float>(vdop);

		// mavlink_log_info(&mavlink_log_pub, "get GSA data ");

	} else if ((memcmp(_rx_buffer + 3, "GSV,", 3) == 0)) {
		/*
		  The GSV message string identifies the number of SVs in view, the PRN numbers, elevations, azimuths, and SNR values. An example of the GSV message string is:

		  $GPGSV,4,1,13,02,02,213,,03,-3,000,,11,00,121,,14,13,172,05*67

		  GSV message fields
		  Field   Meaning
		  0   Message ID $GPGSV
		  1   Total number of messages of this type in this cycle
		  2   Message number
		  3   Total number of SVs visible
		  4   SV PRN number
		  5   Elevation, in degrees, 90 maximum
		  6   Azimuth, degrees from True North, 000 through 359
		  7   SNR, 00 through 99 dB (null when not tracking)
		  8-11    Information about second SV, same format as fields 4 through 7
		  12-15   Information about third SV, same format as fields 4 through 7
		  16-19   Information about fourth SV, same format as fields 4 through 7
		  20  The checksum data, always begins with *
		*/
		bool bGPS = false;

		if (memcmp(_rx_buffer, "$GP", 3) != 0) {
			return 0;

		} else {
			bGPS = true;
		}

		int all_msg_num = 0, this_msg_num = 0, tot_sv_visible = 0;
		struct gsv_sat {
			int svid;
			int elevation;
			int azimuth;
			int snr;
		} sat[4];
		memset(sat, 0, sizeof(sat));

		if (bufptr && *(++bufptr) != ',') { all_msg_num = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { this_msg_num = strtol(bufptr, &endp, 10); bufptr = endp; }

		if (bufptr && *(++bufptr) != ',') { tot_sv_visible = strtol(bufptr, &endp, 10); bufptr = endp; }

		if ((this_msg_num < 1) || (this_msg_num > all_msg_num)) {
			return 0;
		}

		if (this_msg_num == 0 && bGPS && _satellite_info) {
			memset(_satellite_info->svid,     0, sizeof(_satellite_info->svid));
			memset(_satellite_info->used,     0, sizeof(_satellite_info->used));
			memset(_satellite_info->snr,      0, sizeof(_satellite_info->snr));
			memset(_satellite_info->elevation, 0, sizeof(_satellite_info->elevation));
			memset(_satellite_info->azimuth,  0, sizeof(_satellite_info->azimuth));
		}

		int end = 4;

		if (this_msg_num == all_msg_num) {
			end =  tot_sv_visible - (this_msg_num - 1) * 4;
			// _gps_position->satellites_used = tot_sv_visible;

			if (_satellite_info) {
				_satellite_info->count = satellite_info_s::SAT_INFO_MAX_SATELLITES;
				_satellite_info->timestamp = gps_absolute_time();
			}
		}

		if (_satellite_info) {
			for (int y = 0 ; y < end ; y++) {
				if (bufptr && *(++bufptr) != ',') { sat[y].svid = strtol(bufptr, &endp, 10); bufptr = endp; }

				if (bufptr && *(++bufptr) != ',') { sat[y].elevation = strtol(bufptr, &endp, 10); bufptr = endp; }

				if (bufptr && *(++bufptr) != ',') { sat[y].azimuth = strtol(bufptr, &endp, 10); bufptr = endp; }

				if (bufptr && *(++bufptr) != ',') { sat[y].snr = strtol(bufptr, &endp, 10); bufptr = endp; }

				_satellite_info->svid[y + (this_msg_num - 1) * 4]      = sat[y].svid;
				_satellite_info->used[y + (this_msg_num - 1) * 4]      = (sat[y].snr > 0);
				_satellite_info->snr[y + (this_msg_num - 1) * 4]       = sat[y].snr;
				_satellite_info->elevation[y + (this_msg_num - 1) * 4] = sat[y].elevation;
				_satellite_info->azimuth[y + (this_msg_num - 1) * 4]   = sat[y].azimuth;
			}
		}
		// mavlink_log_info(&mavlink_log_pub, "get GSV data ");
		
	} else if ((memcmp(_rx_buffer+ 3, "VTG,", 3) == 0) && (uiCalcComma == 9)) {
                 /*$GPVTG,,T,,M,0.00,N,0.00,K*4E                 

                Field	Meaning
                0	Message ID $GPVTG
                1	Track made good (degrees true)
                2	T: track made good is relative to true north
                3	Track made good (degrees magnetic)
                4	M: track made good is relative to magnetic north
                5	Speed, in knots
                6	N: speed is measured in knots
                7	Speed over ground in kilometers/hour (kph)
                8	K: speed over ground is measured in kph
                9	The checksum data, always begins with *
				*/
 
                float track_true = 0.0;
                char T __attribute__((unused)) ;
                float Mtrack_true __attribute__((unused)) = 0.0;
                char M  __attribute__((unused)) ;
                float ground_speed =0.0; 
                char N  __attribute__((unused));
                float ground_speed_K __attribute__((unused)) = 0.0;
                char K __attribute__((unused));
 
                 if (bufptr && *(++bufptr) != ',') {track_true = strtod(bufptr, &endp); bufptr = endp; }

                 if (bufptr && *(++bufptr) != ',') { T= *(bufptr++); }

                 if (bufptr && *(++bufptr) != ',') {Mtrack_true = strtod(bufptr, &endp); bufptr = endp; }

                 if (bufptr && *(++bufptr) != ',') { M= *(bufptr++); }

                 if(bufptr && *(++bufptr) != ',') { ground_speed = strtod(bufptr,&endp); bufptr = endp; }

                 if(bufptr && *(++bufptr) != ',') { N = *(bufptr++); }

                 if(bufptr && *(++bufptr) != ',') { ground_speed_K = strtod(bufptr, &endp); bufptr= endp; }

                 if(bufptr && *(++bufptr) != ',') { K = *(bufptr++); }

                 float track_rad = static_cast<float>(track_true) * M_PI_F/ 180.0f;
                 float velocity_ms =static_cast<float>(ground_speed) / 1.9438445f;
                 float velocity_north =static_cast<float>(velocity_ms) * cosf(track_rad);
                 float velocity_east  = static_cast<float>(velocity_ms) *sinf(track_rad);

                  _gps_position->vel_m_s = velocity_ms;        
                  _gps_position->vel_n_m_s =velocity_north;   
                  _gps_position->vel_e_m_s =velocity_east;
                  _gps_position->cog_rad =track_rad;  
				  _gps_position->vel_ned_valid = true;				/** Flag to indicate if NED speed is valid */
				  _gps_position->c_variance_rad = 0.1f;
		  
				  _rate_count_vel++;
				// mavlink_log_info(&mavlink_log_pub, "get VTG data ");
 
	}

	if (ret > 0) {
		_gps_position->timestamp_time_relative = (int32_t)(_last_timestamp_time - _gps_position->timestamp);
	}

	return ret;
}

int GPSDriverNMEA::receive(unsigned timeout)
{
	{

		uint8_t buf[GPS_READ_BUFFER_SIZE];

		/* timeout additional to poll */
		uint64_t time_started = gps_absolute_time();
		
		int j = 0;
		ssize_t bytes_count = 0;

		while (true) {

			/* pass received bytes to the packet decoder */
			while (j < bytes_count) {
				int l = 0;

				if ((l = parseChar(buf[j])) > 0) {
					/* return to configure during configuration or to the gps driver during normal work
					 * if a packet has arrived */
					if (handleMessage(l) > 0) {
						return 1;
					}
				}

				j++;
			}

			/* everything is read */
			j = bytes_count = 0;

			/* then poll or read for new data */
			int ret = read(buf, sizeof(buf), timeout * 2);

			if (ret < 0) {
				/* something went wrong when polling */
				return -1;

			} else if (ret == 0) {
				/* Timeout while polling or just nothing read if reading, let's
				 * stay here, and use timeout below. */

			} else if (ret > 0) {
				/* if we have new data from GPS, go handle it */
				bytes_count = ret;
			}

			/* in case we get crap from GPS or time out */
			if (time_started + timeout * 1000 * 2 < gps_absolute_time()) {
				return -1;
			}
		}
	}

}
#define HEXDIGIT_CHAR(d) ((char)((d) + (((d) < 0xA) ? '0' : 'A'-0xA)))

int GPSDriverNMEA::parseChar(uint8_t b)
{
	int iRet = 0;

	switch (_decode_state) {
	/* First, look for sync1 */
        case NMEA_DECODE_UNINIT:
		if (b == '$') {
            _decode_state = NMEA_DECODE_GOT_SYNC1;
			_rx_buffer_bytes = 0;
			_rx_buffer[_rx_buffer_bytes++] = b;
		}

		break;

        case NMEA_DECODE_GOT_SYNC1:
		if (b == '$') {
            _decode_state = NMEA_DECODE_GOT_SYNC1;
			_rx_buffer_bytes = 0;

		} else if (b == '*') {
            _decode_state = NMEA_DECODE_GOT_NMEA;
		}

		if (_rx_buffer_bytes >= (sizeof(_rx_buffer) - 5)) {
            _decode_state = NMEA_DECODE_UNINIT;
			_rx_buffer_bytes = 0;

		} else {
			_rx_buffer[_rx_buffer_bytes++] = b;
		}

		break;

        case NMEA_DECODE_GOT_NMEA:
		_rx_buffer[_rx_buffer_bytes++] = b;
		_decode_state = NMEA_DECODE_GOT_FIRST_CS_BYTE;
		break;

        case NMEA_DECODE_GOT_FIRST_CS_BYTE:
		_rx_buffer[_rx_buffer_bytes++] = b;
		uint8_t checksum = 0;
		uint8_t *buffer = _rx_buffer + 1;
		uint8_t *bufend = _rx_buffer + _rx_buffer_bytes - 3;

		for (; buffer < bufend; buffer++) { checksum ^= *buffer; }

		if ((HEXDIGIT_CHAR(checksum >> 4) == *(_rx_buffer + _rx_buffer_bytes - 2)) &&
		    (HEXDIGIT_CHAR(checksum & 0x0F) == *(_rx_buffer + _rx_buffer_bytes - 1))) {
			iRet = _rx_buffer_bytes;
		}

                _decode_state = NMEA_DECODE_UNINIT;
		_rx_buffer_bytes = 0;
		break;
	}

	return iRet;
}

void GPSDriverNMEA::decodeInit()
{

}

  int GPSDriverNMEA::configure(unsigned &baudrate, OutputMode output_mode)
  {
  	if (output_mode != OutputMode::GPS) {
                  GPS_WARN("NMEA: Unsupported Output Mode %i", (int)output_mode);
 		return -1;
  	}

  			return setBaudrate(_baudrate);
  	}