#ifndef MESSAGES_H
#define MESSAGES_H

// Set DEBUG state for all files.
#define DEBUG_MODE				    0

// Constants
#define END_OF_TRANSMISSION 	    127

#define EPSILON						0.0001
#define NANOSECONDS					1000000000.0
#define MILLISECONDS				1000000.0

// Defaults
#define DEFAUL_PORT				    12345
#define DEFAULT_MSG_LEN			    512
#define DEFAULT_NAME_LENGTH 		26
#define DEFAULT_SOCKET				-1
#define DEFAULT_NUM_ACCOUNTS        64
#define DEFAULT_RANDOM_SEED			42

#define GAME_REVEAL_0			    0
#define GAME_REVEAL_1			    1
#define GAME_REVEAL_2			    2
#define GAME_REVEAL_3			    3
#define GAME_REVEAL_4			    4
#define GAME_REVEAL_5			    5
#define GAME_REVEAL_6			    6
#define GAME_REVEAL_7			    7
#define GAME_REVEAL_8			    8
#define GAME_UNKNOWN			    9
#define GAME_FLAG				    10
#define GAME_MINE				    11

#define NUM_TILES					81
#define NUM_ROWS					9
#define NUM_COLS					9
#define NUM_MINES					10

// Leaderboard Information
#define LEADERBOARD_ENTRIES			10

// Queue Information
#define QUEUE_CLIENT_BUFFER_LEN    	32
#define QUEUE_BUFFERS				160

// Message Headers
#define LEN_TYPE_LOGIN              1
#define LEN_TYPE_ACC                1
#define LEN_TYPE_NOP                1
#define LEN_TYPE_USED				1
#define LEN_TYPE_CON				1
#define LEN_TYPE_QUEUE				1
#define LEN_TYPE_TIME				1
#define LEN_TYPE_START				1
#define LEN_TYPE_GO					1
#define LEN_TYPE_STOP				1
#define LEN_TYPE_FLAG			    1
#define LEN_TYPE_REV			    1
#define LEN_TYPE_LEFT			    1
#define LEN_TYPE_MINE			    1
#define LEN_TYPE_ADJ			    1
#define LEN_TYPE_LEAD_P			    1
#define LEN_TYPE_LEAD_R			    1
#define LEN_TYPE_LEAD_E			    1

static const u8 MESSAGE_TYPE_LOGIN	[] = "a";
static const u8 MESSAGE_TYPE_ACC	[] = "b";
static const u8 MESSAGE_TYPE_NOP	[] = "c";
static const u8 MESSAGE_TYPE_USED	[] = "d";
static const u8 MESSAGE_TYPE_CON    [] = "e";
static const u8 MESSAGE_TYPE_QUEUE  [] = "f";
static const u8 MESSAGE_TYPE_TIME   [] = "g";
static const u8 MESSAGE_TYPE_START  [] = "h";
static const u8 MESSAGE_TYPE_GO     [] = "i";
static const u8 MESSAGE_TYPE_STOP   [] = "j";
static const u8 MESSAGE_TYPE_FLAG   [] = "k";
static const u8 MESSAGE_TYPE_REV    [] = "l";
static const u8 MESSAGE_TYPE_LEFT   [] = "m";
static const u8 MESSAGE_TYPE_MINE   [] = "n";
static const u8 MESSAGE_TYPE_ADJ    [] = "o";
static const u8 MESSAGE_TYPE_LEAD_P [] = "p";
static const u8 MESSAGE_TYPE_LEAD_R [] = "q";
static const u8 MESSAGE_TYPE_LEAD_E [] = "r";

// Message Body Keys
#define LEN_DATA_USERNAME             1
#define LEN_DATA_PASSWORD             1

static const u8 MESSAGE_DATA_USERNAME [] = "w";
static const u8 MESSAGE_DATA_PASSWORD [] = "x";


// Parsers
i8 parse_header(u8** iterator, const u8* target_data, u8 length)
{
	u8* backup_iterator = *iterator; 
	
	// go through word and count matches
	u8 match_count = 0;
	for (u16 i = 0; i < length; i++)
	{
		if (*(*iterator) == END_OF_TRANSMISSION) { break; }
		if (*(*iterator) != target_data[i]) 	 { break; }
		else
		{
			match_count++;
			(*iterator)++;
		}
	}

	// reset iterator if no match
	if (match_count != length)
	{
		*iterator = backup_iterator;
	}
	else if (*(*iterator) == '\n')
	{
		(*iterator)++;
	}

	return match_count == length;
}

i8 parse_data(u8** iterator, u8* data, const u8* target_data, u8 length)
{
	u8* backup_iterator = *iterator;

	// go through word and count matches
	u8 set_index   = 0;
	u8 match_count = 0;
	for (u16 i = 0; i < DEFAULT_MSG_LEN; i++)
	{
		if (*(*iterator) == '\n' || *(*iterator) == END_OF_TRANSMISSION)
		{
			if (*(*iterator) == '\n')
			{
				(*iterator)++;
			}
			break;
		}
		if (match_count == length)
		{
			data[set_index] = *(*iterator);
			set_index++;
		}
		else if (*(*iterator) == target_data[i])
		{
			match_count++;
		}
		(*iterator)++;
	}

	// reset iterator if no match
	if (match_count != length)
	{
		*iterator = backup_iterator;
	}

	return match_count == length;
}

// Timing
void time_diff(struct timespec start, struct timespec end, struct timespec* dt)
{
	if ((end.tv_nsec-start.tv_nsec) < 0) {
		dt->tv_sec  = end.tv_sec - start.tv_sec - 1;
		dt->tv_nsec = NANOSECONDS + end.tv_nsec - start.tv_nsec;
	} else {
		dt->tv_sec  = end.tv_sec  - start.tv_sec;
		dt->tv_nsec = end.tv_nsec - start.tv_nsec;
	}
}

#endif
