// system
#include "stdio.h"
#include "assert.h"
#include "string.h"

#include "unistd.h"
#include "signal.h"
#include "termios.h"
#include "time.h"

#include "stropts.h"
#include "arpa/inet.h"
#include "sys/types.h"
#include "sys/socket.h"
#include "sys/select.h"
#include "pthread.h"

// local
#include "types.h"
#include "common.h"

// defined constants
#define NUM_CONNECT_RETRIES		64
#define DEFAULT_FPS				24.0

#define MENU_MAX				2
#define MENU_PLAY				0
#define MENU_LEADERBOARD		1
#define MENU_QUIT				2

#define STATE_CONNECTING		0b00000000000000000000000000000000
#define STATE_RETRY				0b00000000000000000000000000000001
#define STATE_WAITING			0b00000000000000000000000000000010
#define STATE_USED				0b00000000000000000000000000000100
#define STATE_USERNAME_EDIT		0b00000000000000000000000000001000
#define STATE_PASSWORD_EDIT		0b00000000000000000000000000010000
#define STATE_PASSWORD_SHOW 	0b00000000000000000000000000100000
#define STATE_MENU		     	0b00000000000000000000000001000000
#define STATE_LEADERBOARD		0b00000000000000000000000010000000
#define STATE_GAME				0b00000000000000000000000100000000
#define STATE_WIN				0b00000000000000000000001000000000
#define STATE_LOSE				0b00000000000000000000010000000000

#define ANIMATION_LIMIT			38
#define ANIMATION_SMOOTH		5
#define ANIMATION_CYCLE_LIMIT   32

#define BOLD			"\033[1m"
#define DIM	    		"\033[2m"
#define RESET			"\033[0m"

// defined macros
#define CLEAR_SCREEN() 	printf("\e[1;1H\e[2J")
#define PANIC(e)		CLEAR_SCREEN(); printf("panic: %s\n", e); return 0


// global constants
static const u8 TILE_ROW[] = {"ABCDEFGHI"};


// globals
struct termios  orig_termios;
u32             STATE;
i32             server_sock;
i32             ret_val;
u8 				queue[QUEUE_BUFFERS][DEFAULT_MSG_LEN];
u8 				message_idx = 0;
pthread_t		message_manager;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

u8 exit_flag_connected        = 0;
u8 exit_flag_thread_listening = 0;


// prototypes
i32   keyboard_hit();
i8    connect_to_server(i32 argc, u8** argv);
void  exit_handle();
void* message_handler(void* void_thread_idx);
void  reset_terminal_mode();
void  set_conio_terminal_mode();


// entry point
i32 main(i32 argc, u8** argv)
{
	signal(SIGINT,  exit_handle);
	signal(SIGTERM, exit_handle);

	// connect to server and start message poll
	if(connect_to_server(argc, argv)) { return -1; }
	exit_flag_connected  = 1;
	pthread_create(&message_manager, 0, message_handler, 0);
	exit_flag_thread_listening = 1;
	set_conio_terminal_mode();
	
	// 1/DEFAULT_FPS Hz target
	struct timespec t0;
	struct timespec t1;
	struct timespec dt;
	f64 frame_time_milliseconds		   = 0.0;
	f64 last_frame_time_milliseconds   = 0.0;
	f64 target_frame_time_milliseconds = (DEFAULT_FPS > 0.0) ? (1000.0 / DEFAULT_FPS) : 0.0;

	u16 animation_counter     = 0;
	u8  animation_phase       = 0;
	u8  animation_cycles      = 1;

	u8  temp                  = 0;
	u8  mines_left  		  = NUM_MINES;
	u8  menu_cursor   		  = 0;
	u8  game_cursor			  = NUM_TILES / 2;;
	u8* target_field          = 0;
	u8  name_index 			  = 0;
	u16 queue_position		  = 0;
	u16 login_fails			  = 0;
	f64 time_elapsed		  = 0;

	u8  leaderboard_usernames[LEADERBOARD_ENTRIES][DEFAULT_MSG_LEN] = {0};
	i64 leaderboard_seconds[LEADERBOARD_ENTRIES]                    = {0};
	i64 leaderboard_nano[LEADERBOARD_ENTRIES]                       = {0};
	u32 leaderboard_games_won[LEADERBOARD_ENTRIES] 	                = {0};
	u32 leaderboard_games_played[LEADERBOARD_ENTRIES]               = {0};
	u16 page_number = 0;

	u8 msg[DEFAULT_MSG_LEN]		     = {0};
	u8 username[DEFAULT_NAME_LENGTH] = {0};
	u8 password[DEFAULT_NAME_LENGTH] = {0};
	u8 game_map[NUM_TILES];
	for(u8 i = 0; i < NUM_TILES; i++)
	{
		game_map[i] = GAME_UNKNOWN;
	}

	// begin poll
	#if DEBUG_MODE
		STATE = STATE_MENU; 
	#else
		STATE = STATE_USERNAME_EDIT; 
	#endif
	while (1)
	{
		clock_gettime(CLOCK_MONOTONIC, &t0);
		
		// input + logic
		if (keyboard_hit())
		{
			// catch sigint
			temp = getchar();
			if (temp == 3)
			{
				exit_handle();
			}

			if (!(STATE & STATE_WAITING))
			{
				if (STATE & STATE_USERNAME_EDIT || STATE & STATE_PASSWORD_EDIT)
				{
					if (STATE & STATE_USERNAME_EDIT)
					{
						target_field = username;
					}
					else if (STATE & STATE_PASSWORD_EDIT)
					{
						target_field = password;
					}

					// ansi sequences
					if (temp == '\033')
					{
						temp = getchar(); // ignored this one
						temp = getchar(); // keep this one

						// delete
						if (temp == 51)
						{
							temp = getchar(); // ignored
							if (name_index < DEFAULT_NAME_LENGTH)
							{
								if (name_index == DEFAULT_NAME_LENGTH - 1)
								{
									target_field[name_index] = 0;
								}
								else
								{
									for (u8 i = name_index; i < DEFAULT_NAME_LENGTH - 1; i++)
									{
										target_field[i] = target_field[i + 1];
									}
								}
							}
						}

						// right arrow
						if (temp == 67)
						{
							if (name_index + 1 <= DEFAULT_NAME_LENGTH && target_field[name_index] != 0)
							{
								name_index += 1;
							}
						}

						// left arrow
						else if (temp == 68)
						{
							if (name_index - 1 >= 0)
							{
								name_index -= 1;
							}
						}
					}

					// tab
					else if (temp == 9)
					{
						if (STATE & STATE_PASSWORD_EDIT)
						{
							if (STATE & STATE_PASSWORD_SHOW)
							{
								STATE &= ~STATE_PASSWORD_SHOW;
							}
							else
							{
								STATE |= STATE_PASSWORD_SHOW;
							}
						}
					}

					// enter
					else if (temp == 10 || temp == 13)
					{
						for (u16 i = 0; i < DEFAULT_NAME_LENGTH; i++)
						{
							if (target_field[i] == 0)
							{
								memset(&target_field[i], 0, DEFAULT_NAME_LENGTH - i);
								break;
							}
						}

						target_field = 0;
						if (STATE & STATE_USERNAME_EDIT)
						{
							name_index = 0;
							STATE &= ~STATE_USERNAME_EDIT;
							STATE |= STATE_PASSWORD_EDIT;
						}
						else if (!(STATE & STATE_WAITING) && (STATE & STATE_PASSWORD_EDIT))
						{
							// STATE &= ~STATE_PASSWORD_EDIT;
							// STATE |= STATE_WAITING;
							STATE = STATE_WAITING;

							// construct login request message
							u8* msg_pointer = msg;

							memcpy(msg_pointer, MESSAGE_TYPE_LOGIN, sizeof(MESSAGE_TYPE_LOGIN));
							msg_pointer += sizeof(MESSAGE_TYPE_LOGIN) - 1;
							*msg_pointer = '\n';
							msg_pointer++;

							memcpy(msg_pointer, MESSAGE_DATA_USERNAME, sizeof(MESSAGE_DATA_USERNAME));
							msg_pointer += sizeof(MESSAGE_DATA_USERNAME) - 1;
							memcpy(msg_pointer, username, DEFAULT_NAME_LENGTH);
							msg_pointer += DEFAULT_NAME_LENGTH;
							*msg_pointer = '\n';
							msg_pointer++;

							memcpy(msg_pointer, MESSAGE_DATA_PASSWORD, sizeof(MESSAGE_DATA_PASSWORD));
							msg_pointer += sizeof(MESSAGE_DATA_PASSWORD) - 1;
							memcpy(msg_pointer, password, DEFAULT_NAME_LENGTH);
							msg_pointer += DEFAULT_NAME_LENGTH;
							*msg_pointer = END_OF_TRANSMISSION;

							ret_val = send(server_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL); 
							if (ret_val < 0)
							{
								printf("\rFailed to send login information to server.\r\n");
								break;
							}
						}
					}

					// backspace
					else if (temp == 127)
					{
						if (name_index > 0)
						{
							if (name_index == DEFAULT_NAME_LENGTH)
							{
								target_field[name_index-1] = 0;
							}
							else
							{
								for (u8 i = name_index - 1; i < DEFAULT_NAME_LENGTH - 1; i++)
								{
									target_field[i] = target_field[i + 1];
								}
							}
							name_index -= 1;
						}
						else if (STATE & STATE_PASSWORD_EDIT)
						{
							STATE &= ~STATE_PASSWORD_EDIT;
							STATE |= STATE_USERNAME_EDIT;
							name_index = DEFAULT_NAME_LENGTH;
							for (u8 i = 0; i < DEFAULT_NAME_LENGTH; i++)
							{
								if (username[i] == 0)
								{
									name_index = i;
									break;
								}
							}
						}
					}

					// numbers, capitals, letters, symbols
					else if (temp >= 32 && temp <= 126)
					{
						if (name_index < DEFAULT_NAME_LENGTH)
						{
							for (u8 i = DEFAULT_NAME_LENGTH - 1; i > name_index; i--)
							{
								target_field[i] = target_field[i-1];
							}
							target_field[name_index] = temp;
							name_index += 1;
						}
					}
				}
				else if (STATE & STATE_MENU)
				{
					// ansi sequences
					if (temp == '\033')
					{
						temp = getchar(); // ignored this one
						temp = getchar(); // keep this one

						// up arrow
						if (temp == 65)
						{
							if (menu_cursor == 0)
							{
								menu_cursor = MENU_MAX;
							}
							else
							{
								menu_cursor--;
							}
						}

						// down arrow
						else if (temp == 66)
						{
							if (menu_cursor == MENU_MAX)
							{
								menu_cursor = 0;
							}
							else
							{
								menu_cursor++;
							}
						}
					}

					// enter
					else if (temp == 10 || temp == 13)
					{
						switch (menu_cursor)
						{
							case MENU_PLAY:
								STATE = STATE_GAME | STATE_WAITING;
								for (u16 i = 0; i < LEN_TYPE_START; i++)
								{
									msg[i] = MESSAGE_TYPE_START[i];
								}
								msg[LEN_TYPE_START] = END_OF_TRANSMISSION;
								send(server_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
								break; 
							case MENU_LEADERBOARD: 
								STATE = STATE_LEADERBOARD;
								for (u16 i = 0; i < LEN_TYPE_LEAD_P; i++)
								{
									msg[i] = MESSAGE_TYPE_LEAD_P[i];
								}
								msg[LEN_TYPE_LEAD_P]   = page_number >> 8;
								msg[LEN_TYPE_LEAD_P+1] = page_number;
								msg[LEN_TYPE_LEAD_P+2] = END_OF_TRANSMISSION;
								send(server_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
								break; 
							case MENU_QUIT:
								exit_handle();
								break; 
						}
					}
				}
				else if (STATE & STATE_GAME)
				{
					// ansi sequences
					if (temp == '\033')
					{
						temp = getchar(); // ignored this one
						temp = getchar(); // keep this one

						// del
						if (temp == 51)
						{
							STATE       = STATE_MENU;
							game_cursor = NUM_TILES / 2;
							mines_left  = NUM_MINES;
							for (u8 i = 0; i < NUM_TILES; i++)
							{
								game_map[i] = GAME_UNKNOWN;
							}

							if (!(STATE & STATE_WIN) && !(STATE & STATE_LOSE))
							{
								for (u16 i = 0; i < LEN_TYPE_STOP; i++)
								{
									msg[i] = MESSAGE_TYPE_STOP[i];
								}
								msg[LEN_TYPE_STOP] = END_OF_TRANSMISSION;
								send(server_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
							}
						}

						else if (!(STATE & STATE_WIN) && !(STATE & STATE_LOSE))
						{
							// up arrow
							if (temp == 65)
							{
								if (game_cursor / NUM_ROWS == 0)
								{
									game_cursor = NUM_TILES - NUM_COLS + game_cursor;
								}
								else
								{
									game_cursor -= NUM_COLS;
								}
							}

							// down arrow
							else if (temp == 66)
							{
								if (game_cursor / NUM_ROWS == NUM_ROWS - 1	)
								{
									game_cursor -= NUM_TILES - NUM_COLS;
								}
								else
								{
									game_cursor += NUM_COLS;
								}
							}

							// right arrow
							else if (temp == 67)
							{
								if (game_cursor % NUM_COLS == NUM_COLS - 1)
								{
									game_cursor -= NUM_COLS - 1;
								}
								else
								{
									game_cursor++;
								}
							}

							// left arrow
							else if (temp == 68)
							{
								if (game_cursor % NUM_COLS == 0)
								{
									game_cursor += NUM_COLS - 1;
								}
								else
								{
									game_cursor--;
								}
							}
						}
					}

					else if (!(STATE & STATE_WIN) && !(STATE & STATE_LOSE))
					{
						// enter
						if (game_map[game_cursor] == GAME_UNKNOWN && (temp == 10  || temp == 13))
						{
							for (u16 i = 0; i < LEN_TYPE_REV; i++)
							{
								msg[i] = MESSAGE_TYPE_REV[i];
							}
							msg[LEN_TYPE_REV]     = game_cursor;
							msg[LEN_TYPE_REV + 1] = END_OF_TRANSMISSION;
							send(server_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
						}

						// space
						else if (game_map[game_cursor] > GAME_REVEAL_8  && temp == 32)
						{
							if (game_map[game_cursor] == GAME_UNKNOWN) 
							{
								game_map[game_cursor] = GAME_FLAG;
							}
							else
							{
								game_map[game_cursor] = GAME_UNKNOWN;
							}
							for (u16 i = 0; i < LEN_TYPE_FLAG; i++)
							{
								msg[i] = MESSAGE_TYPE_FLAG[i];
							}
							msg[LEN_TYPE_FLAG]      = game_cursor;
							msg[LEN_TYPE_FLAG + 1]  = END_OF_TRANSMISSION;
							send(server_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
						}

						// alternative controls -------------------------------------------------
						
						// letters
						else if ((temp > 64 && temp <= 64 + NUM_ROWS) || (temp > 96 && temp <= 96 + NUM_ROWS))
						{
							if (temp < 97) { temp += 32; }
							game_cursor = (game_cursor % NUM_COLS) + ((temp - 97) * NUM_COLS);
						}

						// numbers
						else if (temp > 48 && temp <= 48 + NUM_COLS)
						{
							game_cursor = ((game_cursor / NUM_ROWS) * NUM_ROWS) + (temp - 49);
						}

						// tab - set all remaining tiles to flags
						#if DEBUG_MODE
						else if (temp == 9)
						{
							// prepare message
							for (u16 j = 0; j < LEN_TYPE_FLAG; j++)
							{
								msg[j] = MESSAGE_TYPE_FLAG[j];
							}
							msg[LEN_TYPE_FLAG + 1]  = END_OF_TRANSMISSION;

							// send
							for (u8 i = 0; i < NUM_TILES; i++)
							{
								if (game_map[i] > GAME_REVEAL_8)
								{
									if (game_map[i] == GAME_UNKNOWN) 
									{
										game_map[i] = GAME_FLAG;
									}
									else
									{
										game_map[i] = GAME_UNKNOWN;
									}
									msg[LEN_TYPE_FLAG] = i;
									send(server_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
								}
							}
						}
						#endif
					} 
				}
				else if (STATE & STATE_LEADERBOARD)
				{
					// ansi sequences
					if (temp == '\033')
					{
						temp = getchar(); // ignored this one
						temp = getchar(); // keep this one

						u8 next_page_request = 0;

						// down or right arrow - next page
						if ((temp == 66 || temp == 67) && page_number != 0)
						{
							page_number--;
							next_page_request = 1;
						}

						// up or left arrow - previous page
						else if ((temp == 65 || temp == 68) && 
							page_number + 1 < DEFAULT_NUM_ACCOUNTS && 
							leaderboard_usernames[LEADERBOARD_ENTRIES-1][0] != 0)
						{
							page_number++;
							next_page_request = 1;
						}

						if (next_page_request)
						{
							for (u16 i = 0; i < LEN_TYPE_LEAD_P; i++)
							{
								msg[i] = MESSAGE_TYPE_LEAD_P[i];
							}
							msg[LEN_TYPE_LEAD_P]   = page_number >> 8;
							msg[LEN_TYPE_LEAD_P+1] = page_number;
							msg[LEN_TYPE_LEAD_P+2] = END_OF_TRANSMISSION;
							send(server_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
						}
					}

					// enter
					else if (temp == 10 || temp == 13)
					{
						STATE = STATE_MENU;
						page_number = 0;
					}
				}
			}
		}

		// parse message queue
		for (u8 i = 0; i < QUEUE_BUFFERS; i++)
		{
			pthread_mutex_lock(&queue_mutex);
			// break on empty message
			if (queue[i][0] == 0) 
			{ 
				pthread_mutex_unlock(&queue_mutex);
				break; 
			}

			// grab message from queue
			for (u16 j = 0; j < DEFAULT_MSG_LEN; j++)
			{
				msg[j] = queue[i][j];
			}
			u8* msg_pointer = msg;
			pthread_mutex_unlock(&queue_mutex);

			// parse message
			if(parse_header(&msg_pointer, MESSAGE_TYPE_QUEUE, LEN_TYPE_QUEUE))
			{
				STATE 		   |= STATE_WAITING;
				queue_position  = msg[LEN_TYPE_QUEUE] << 8;
				queue_position |= msg[LEN_TYPE_QUEUE+1];
				queue_position++;
			}
			else if(parse_header(&msg_pointer, MESSAGE_TYPE_TIME, LEN_TYPE_TIME))
			{
				u64 dt_sec = 0;
				dt_sec |= (u64)msg[LEN_TYPE_TIME + 0] << 56; 
				dt_sec |= (u64)msg[LEN_TYPE_TIME + 1] << 48; 
				dt_sec |= (u64)msg[LEN_TYPE_TIME + 2] << 40;
				dt_sec |= (u64)msg[LEN_TYPE_TIME + 3] << 32;
				dt_sec |= (u64)msg[LEN_TYPE_TIME + 4] << 24;
				dt_sec |= (u64)msg[LEN_TYPE_TIME + 5] << 16;
				dt_sec |= (u64)msg[LEN_TYPE_TIME + 6] << 8;
				dt_sec |= (u64)msg[LEN_TYPE_TIME + 7];

				u64 dt_nano = 0; 
				dt_nano |= (u64)msg[LEN_TYPE_TIME + 8 ] << 56; 
				dt_nano |= (u64)msg[LEN_TYPE_TIME + 9 ] << 48; 
				dt_nano |= (u64)msg[LEN_TYPE_TIME + 10] << 40;
				dt_nano |= (u64)msg[LEN_TYPE_TIME + 11] << 32;
				dt_nano |= (u64)msg[LEN_TYPE_TIME + 12] << 24;
				dt_nano |= (u64)msg[LEN_TYPE_TIME + 13] << 16;
				dt_nano |= (u64)msg[LEN_TYPE_TIME + 14] << 8;
				dt_nano |= (u64)msg[LEN_TYPE_TIME + 15];

				i64 sign_dt_sec   = (i64) dt_sec; 
        		i64 sign_dt_nano  = (i64) dt_nano; 

				time_elapsed  = sign_dt_sec;
        		time_elapsed += (f64)(sign_dt_nano / NANOSECONDS);
			}
			else if(parse_header(&msg_pointer, MESSAGE_TYPE_CON, LEN_TYPE_CON))
			{
				STATE &= ~STATE_WAITING;
				queue_position = 0;
			}
			else if(parse_header(&msg_pointer, MESSAGE_TYPE_ACC, LEN_TYPE_ACC))
			{
				STATE = STATE_MENU;
				login_fails = 0;
			}
			else if (parse_header(&msg_pointer, MESSAGE_TYPE_NOP, LEN_TYPE_NOP))
			{
				// reset fields
				u8 set 		 = 0;
				target_field = 0;
				name_index 	 = DEFAULT_NAME_LENGTH;
				STATE        = STATE_USERNAME_EDIT | STATE_RETRY;

				// search for end of username to set index
				for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
				{
					password[j] = 0;
					if (!set && username[j] == 0)
					{
						name_index = j;
						set = 1;
					}
				}
			}
			else if (parse_header(&msg_pointer, MESSAGE_TYPE_USED, LEN_TYPE_USED))
			{
				// reset fields
				u8 set 		 = 0;
				target_field = 0;
				name_index 	 = DEFAULT_NAME_LENGTH;
				STATE        = STATE_USERNAME_EDIT | STATE_USED;

				// search for end of username to set index
				for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
				{
					password[j] = 0;
					if (!set && username[j] == 0)
					{
						name_index = j;
						set = 1;
					}
				}
			}
			else if (parse_header(&msg_pointer, MESSAGE_TYPE_GO, LEN_TYPE_GO))
			{
				STATE 			   = STATE_GAME;
				time_elapsed 	   = 0;
				animation_counter  = 0;
				animation_phase    = 0;
				animation_cycles   = 1;
			}
			else if (parse_header(&msg_pointer, MESSAGE_TYPE_LEFT, LEN_TYPE_LEFT))
			{
				mines_left = msg[LEN_TYPE_LEFT];
				if (mines_left == 0)
				{
					STATE |= STATE_WIN;
				}
			}
			else if (parse_header(&msg_pointer, MESSAGE_TYPE_MINE, LEN_TYPE_MINE))
			{
				STATE |= STATE_LOSE;
				game_map[game_cursor] = GAME_MINE;
			}
			else if (parse_header(&msg_pointer, MESSAGE_TYPE_ADJ, LEN_TYPE_ADJ))
			{
				// set map
				for (u8 i = 0; i < NUM_TILES; i++)
				{
					game_map[i] = *msg_pointer;
					msg_pointer++;
				}
			}
			else if (parse_header(&msg_pointer, MESSAGE_TYPE_LEAD_R, LEN_TYPE_LEAD_R))
			{
				// leaderboard page query result
				for (u8 j = 0; j < LEADERBOARD_ENTRIES; j++)
				{
					// for each username entry, parse seconds and nanoseconds too
					if(parse_data(&msg_pointer, leaderboard_usernames[j], MESSAGE_DATA_USERNAME, LEN_DATA_USERNAME))
					{
						// seconds
						u64 dt_sec = 0;
						dt_sec |= (u64)*msg_pointer << 56; msg_pointer++;
						dt_sec |= (u64)*msg_pointer << 48; msg_pointer++;
						dt_sec |= (u64)*msg_pointer << 40; msg_pointer++;
						dt_sec |= (u64)*msg_pointer << 32; msg_pointer++;
						dt_sec |= (u64)*msg_pointer << 24; msg_pointer++;
						dt_sec |= (u64)*msg_pointer << 16; msg_pointer++;
						dt_sec |= (u64)*msg_pointer << 8;  msg_pointer++;
						dt_sec |= (u64)*msg_pointer;       msg_pointer++;
						msg_pointer++;

						leaderboard_seconds[j] = (i64) dt_sec; 

						// nano-seconds
						u64 dt_nano = 0; 
						dt_nano |= (u64)*msg_pointer << 56; msg_pointer++;
						dt_nano |= (u64)*msg_pointer << 48; msg_pointer++;
						dt_nano |= (u64)*msg_pointer << 40; msg_pointer++;
						dt_nano |= (u64)*msg_pointer << 32; msg_pointer++;
						dt_nano |= (u64)*msg_pointer << 24; msg_pointer++;
						dt_nano |= (u64)*msg_pointer << 16; msg_pointer++;
						dt_nano |= (u64)*msg_pointer << 8;  msg_pointer++;
						dt_nano |= (u64)*msg_pointer;       msg_pointer++;
						msg_pointer++;
						
						leaderboard_nano[j] = (i64) dt_nano; 

						// played
						leaderboard_games_played[j]  = 0;
						leaderboard_games_played[j] |= (u32)*msg_pointer << 24; msg_pointer++;
						leaderboard_games_played[j] |= (u32)*msg_pointer << 16; msg_pointer++;
						leaderboard_games_played[j] |= (u32)*msg_pointer << 8;  msg_pointer++;
						leaderboard_games_played[j] |= (u32)*msg_pointer;       msg_pointer++;
						msg_pointer++;

						// won
						leaderboard_games_won[j]  = 0;
						leaderboard_games_won[j] |= (u32)*msg_pointer << 24; msg_pointer++;
						leaderboard_games_won[j] |= (u32)*msg_pointer << 16; msg_pointer++;
						leaderboard_games_won[j] |= (u32)*msg_pointer << 8;  msg_pointer++;
						leaderboard_games_won[j] |= (u32)*msg_pointer;       msg_pointer++;
						msg_pointer++;
					}
					else
					{
						// zero out remainder
						leaderboard_seconds       [j] = 0;
						leaderboard_nano 	      [j] = 0;
						leaderboard_games_played  [j] = 0;
						leaderboard_games_won 	  [j] = 0;
						for (u8 k = 0; k < DEFAULT_NAME_LENGTH; k++)
						{
							leaderboard_usernames[j][k] = 0;
						}
					}
				}
			}
			else if (parse_header(&msg_pointer, MESSAGE_TYPE_LEAD_E, LEN_TYPE_LEAD_E))
			{
				// leaderboard empty - cant increment
				if (page_number)
				{
					page_number--;
				}
			}

			// clear message
			pthread_mutex_lock(&queue_mutex);
			for (u16 j = 0; j < DEFAULT_MSG_LEN; j++)
			{
				if (queue[i][j] == 0) { break; }
				else { queue[i][j] = 0; }
			}
			pthread_mutex_unlock(&queue_mutex);
		}

		pthread_mutex_lock(&queue_mutex);
		message_idx = 0;
		pthread_mutex_unlock(&queue_mutex);

		// draw
		if (last_frame_time_milliseconds >= target_frame_time_milliseconds)
		{
			CLEAR_SCREEN();
			if (STATE & STATE_USERNAME_EDIT || STATE & STATE_PASSWORD_EDIT)
			{
				printf("\r\n\r\n\r\n");
				printf("                M I N E S W E E P E R\r\n");
				printf("           Enter Your Login Details Below\r\n");
				printf("\r\n\r\n\r\n");

				printf("\r\n");
				printf("            User Name        ");
				for (u8 i = 0; i < DEFAULT_NAME_LENGTH; i++)
				{
					if (STATE & STATE_USERNAME_EDIT && name_index == i)
					{
						printf("|");
					}
					if (username[i] == 0)
					{
						printf(" ");
					}
					else
					{
						printf("%c", username[i]);
					}
				}
				if (STATE & STATE_USERNAME_EDIT && name_index >= DEFAULT_NAME_LENGTH)
				{
					printf("|");
				}
				printf("\r\n            Password         ");
				for (u8 i = 0; i < DEFAULT_NAME_LENGTH; i++)
				{
					if (STATE & STATE_PASSWORD_EDIT && name_index == i)
					{
						printf("|");
					}
					if (STATE & STATE_PASSWORD_SHOW)
					{
						if (password[i] == 0)
						{
							printf(" ");
						}
						else
						{
							printf("%c", password[i]);
						}
					}
					else if (password[i] != 0)
					{
						printf("*");
					}
				}
				if (STATE & STATE_PASSWORD_EDIT && name_index >= DEFAULT_NAME_LENGTH)
				{
					printf("|");
				}
				printf("\r\n");

				if (STATE & STATE_RETRY)
				{
					printf("\r\n\r\n\r\n\r\n\r\n");
					printf("                    Login Failed.\r\n\r\n");
					printf("          Use <TAB> To Toggle Password Show\r\n");
					printf("\r\n\r\n\r\n\r\n");
				}
				else if (STATE & STATE_WAITING)
				{
					printf("\r\n\r\n\r\n\r\n\r\n");
					printf("                  Waiting in Queue.\r\n\r\n");
					printf("                Current Position: %u\r\n", queue_position);
					printf("\r\n\r\n\r\n\r\n");
				}
				else if (STATE & STATE_USED)
				{
					printf("\r\n\r\n\r\n\r\n\r\n");
					printf("               Credentials Already in Use.\r\n\r\n");
					printf("         [Reminder] Never Share Your Password \r\n");
					printf("\r\n\r\n\r\n\r\n");
				}
				else
				{
					printf("\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
				}

				#if DEBUG_MODE
				printf("-------------------------------------------------------------");
				printf("\r\n\r\n\r\n");
				printf("state           -   %u\r\n", STATE);
				printf("name_index      -   %u\r\n", name_index);
				printf("target_field    -   %p\r\n", target_field);
				printf("queue_position  -   %u\r\n", queue_position);
				if (temp == '\n')
				{
					printf("last_char       -   \"\\n\" - %u\r\n", temp);
				}
				else if (temp == '\r')
				{
					printf("last_char       -   \"\\r\" - %u\r\n", temp);
				}
				else
				{
					printf("last_char       -   \"%c\" - %u\r\n", temp, temp);
				}
				printf("\r\n");
				printf("username: ");
				for (u32 i = 0; i < DEFAULT_NAME_LENGTH; i++)
					printf("%c", username[i]);
				printf("\r\n");
				printf("password: ");
				for (u32 i = 0; i < DEFAULT_NAME_LENGTH; i++)
					printf("%c", password[i]);
				printf("\r\n\r\n\r\n\r\n");
				#endif
			}
			else if (STATE & STATE_WAITING)
			{
				printf("\r\n\r\n\r\n");
				printf("                M I N E S W E E P E R\r\n");
				printf("\r\n\r\n\r\n");

				printf("             Waiting for Server Response.\r\n\r\n");
				printf("      ");
				
				if (!animation_phase)
				{
					// fill up
					for (u8 i = 0; i < animation_counter / ANIMATION_SMOOTH; i++)
					{
						printf(".");
					}
				}
				else
				{
					// fade away
					for (u8 i = 0; i < animation_counter / ANIMATION_SMOOTH; i++)
					{
						printf(" ");
					}
					for (u8 i = 0; i < ANIMATION_LIMIT - (animation_counter / ANIMATION_SMOOTH); i++)
					{
						printf(".");
					}
				}
				printf("      \r\n\r\n\r\n\r\n\r\n\r\n");
				if ((animation_counter + 1) / ANIMATION_SMOOTH < ANIMATION_LIMIT)
				{
					animation_counter++;
				}
				else
				{
					animation_phase   = ~animation_phase;
					animation_counter = 0;
					animation_cycles++;
					if (animation_cycles / 2 > ANIMATION_CYCLE_LIMIT)
						exit_handle();
				}			
			}
			else if (STATE & STATE_MENU)
			{
				printf("\r\n\r\n\r\n");
				printf("                M I N E S W E E P E R");
				printf("\r\n\r\n\r\n");
				if (menu_cursor == MENU_PLAY)
				{
					printf(BOLD "             >          Play\r\n" RESET);
				}
				else
				{
					printf(DIM  "                        Play\r\n" RESET);
				}
				if (menu_cursor == MENU_LEADERBOARD)
				{
					printf(BOLD "             >      Leaderboards\r\n" RESET);
				}
				else
				{
					printf(DIM  "                    Leaderboards\r\n" RESET);
				}
				if (menu_cursor == MENU_QUIT)
				{
					printf(BOLD "             >          Exit\r\n" RESET);
				}
				else
				{
					printf(DIM  "                        Exit\r\n" RESET);
				}
				printf("\r\n\r\n\r\n\r\n");

				#if DEBUG_MODE
				printf("-------------------------------------------------------------");
				printf("\r\n\r\n\r\n");
				printf("cursor          -   %u\r\n", menu_cursor);
				printf("\r\n\r\n\r\n");
				#endif
			}
			else if (STATE & STATE_GAME)
			{
				u16 test_idx = 0;
				printf("\r\n\r\n\r\n");
				printf("                M I N E S W E E P E R");
				printf("\r\n\r\n\r\n");
				printf(DIM "             Time Elapsed: %.2f seconds \r\n\r\n\r\n" RESET, time_elapsed);
				for (u8 i = 0; i < NUM_ROWS + 1; i++)
				{
					if (i == NUM_ROWS)
					{
						// box drawing
						printf("       ");
						for (u8 j = 0; j < NUM_COLS + 2; j++)
						{
							if (j == 0)
							{
								printf("└─");
							}
							else
							{
								printf("──");
							}
						}
						printf("\r\n");

						// headings
						printf("          ");
						for (u8 j = 0; j < NUM_COLS; j++)
						{
							if (game_cursor % NUM_COLS == j)
							{
								printf(BOLD "%d " RESET, j+1);
							}
							else
							{
								printf(DIM  "%d " RESET, j+1);
							}
						}
						printf("\r\n");
					}
					else
					{
						// map
						if (game_cursor / NUM_ROWS == i)
						{
							printf(BOLD "    %c " RESET " │  ", TILE_ROW[i]);
						}
						else
						{
							printf(DIM  "    %c " RESET " │  ", TILE_ROW[i]);
						}
						for (u8 j = 0; j < NUM_COLS; j++)
						{
							u8 tile_idx = (i * NUM_ROWS) + j;

							// font style
							if (game_cursor == tile_idx || game_map[tile_idx] == GAME_FLAG)
							{
								printf(BOLD);
							}
							else 
							{
								printf(DIM);
							}

							// content
							if (game_map[tile_idx] == GAME_UNKNOWN)
							{
								printf("+ ");
							}
							else if (game_map[tile_idx] == GAME_MINE)
							{
								printf("☀ ");
							}
							else if (game_map[tile_idx] == GAME_FLAG)
							{
								printf("x ");
							}
							else if (game_map[tile_idx] != GAME_REVEAL_0)
							{
								printf("%u ", game_map[tile_idx]);
							}
							else if (tile_idx == game_cursor)
							{
								printf("@ ");
							}
							else
							{
								printf("  ");
							}

							printf(RESET);
						}
							
						// controls
						switch (i)
						{
							case 1:
								printf("         %u Mines Left", mines_left);
								break;
							case 5:
								printf("       ENTER │ Reveal Tile");
								break;
							case 6:
								printf("       SPACE │ Place Flag");
								break;
							case 7:
								printf("        DEL  │ Abandon Game");
								break;
						}
						printf("\r\n");
					}
				}

				if (STATE & STATE_WIN)
				{
					printf("\r\n\r\n" DIM);
					printf("                    ┌──────────┐\r\n");
					printf("                    │ " RESET BOLD "You Won!" RESET DIM" │\r\n");
					printf("                    └──────────┘\r\n");
					printf(RESET);
				}
				else if (STATE & STATE_LOSE)
				{
					printf("\r\n\r\n" DIM);
					printf("                    ┌──────────┐\r\n");
					printf("                    │ " RESET BOLD "You Lost" RESET DIM" │\r\n");
					printf("                    └──────────┘\r\n");
					printf(RESET);
				}
				else
				{
					printf("\r\n\r\n\r\n\r\n\r\n");
				}

				#if DEBUG_MODE
				printf("-------------------------------------------------------------");
				printf("\r\n\r\n\r\n");
				printf("cursor          -   %u -> %u %u\r\n", game_cursor, 
					game_cursor % NUM_COLS, game_cursor / NUM_ROWS);
				printf("mines left      -   %u\r\n", mines_left);
				printf("queue idx       -   %u\r\n", message_idx);
				if (temp == '\n')
				{
					printf("last pressed    -  \"\\n\" - %u\r\n", temp);
				}
				else if (temp == '\r')
				{
					printf("last pressed    -  \"\\r\" - %u\r\n", temp);
				}
				else 
				{
					printf("last pressed    -  \"%c\" - %u\r\n", temp, temp);
				}
				printf("\r\n\r\n\r\n");
				#endif
			}
			else if (STATE & STATE_LEADERBOARD)
			{	
				printf("\r\n\r\n\r\n");
				printf("                M I N E S W E E P E R");
				printf("\r\n\r\n\r\n");
				printf("  ─────────────────────────────────────────────────\r\n");
				printf("  User                        Win   Played  Best\r\n");
				printf("  ─────────────────────────────────────────────────\r\n");
				f64 time_elapsed;
				for (u8 i = 0; i < LEADERBOARD_ENTRIES; i++)
				{
					if(i == (LEADERBOARD_ENTRIES / 2) - 2 && leaderboard_usernames[0][0] == 0)
					{
						printf("          No records have currently been set!\r\n");
					}
					else if(i == (LEADERBOARD_ENTRIES / 2) && leaderboard_usernames[0][0] == 0)
					{
						printf("                    Get playing!\r\n");
					}
					else if (leaderboard_usernames[i][0] == 0)
					{
						printf("\r\n");
					}
					else
					{
						// username
						printf("  ");
						for(u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
						{
							if (leaderboard_usernames[i][j] < 32 || leaderboard_usernames[i][j] > 126)
							{
								printf(" ");
							}
							else
							{
								printf("%c", leaderboard_usernames[i][j]);
							}
						}

						// wins
						{
							if (leaderboard_games_won[i] < 10)
							{
								printf("  %u    ", leaderboard_games_won[i]);
							}
							else if (leaderboard_games_won[i] < 100)
							{
								printf("  %u   ", leaderboard_games_won[i]);
							}
							else if (leaderboard_games_won[i] < 1000)
							{
								printf("  %u  ", leaderboard_games_won[i]);
							}
							else
							{
								printf("  %u ", leaderboard_games_won[i]);
							}
						}

						// played
						{
							if (leaderboard_games_played[i] < 10)
							{
								printf("  %u    ", leaderboard_games_played[i]);
							}
							else if (leaderboard_games_played[i] < 100)
							{
								printf("  %u   ", leaderboard_games_played[i]);
							}
							else if (leaderboard_games_played[i] < 1000)
							{
								printf("  %u  ", leaderboard_games_played[i]);
							}
							else
							{
								printf("  %u ", leaderboard_games_played[i]);
							}
						}

						// time
						time_elapsed  = leaderboard_seconds[i];
						time_elapsed += (f64)(leaderboard_nano[i] / NANOSECONDS);
						printf("  %.2f\r\n", time_elapsed);
					}
				}
				printf("  ─────────────────────────────────────────────────\r\n");
				printf("\r\n");
				printf(DIM  "                        Page %u" RESET, page_number + 1);
				printf("\r\n\r\n");
				printf(BOLD "                         Back\r\n" RESET);
				printf("\r\n\r\n");
			}
			else
			{
				#if DEBUG_MODE
					// usually this means the server disconnected, but just in case it doesnt
					printf("State Not Handled\r\n");
					printf("state -> %u\r\n", STATE);
				#else
					exit_handle();
				#endif
			}

			last_frame_time_milliseconds -= target_frame_time_milliseconds;
		}

		last_frame_time_milliseconds += frame_time_milliseconds;
		
		clock_gettime(CLOCK_MONOTONIC, &t1);
		time_diff(t0, t1, &dt);

		frame_time_milliseconds  = dt.tv_sec * 1000;
        frame_time_milliseconds += (f64)(dt.tv_nsec / MILLISECONDS); 
	}

	shutdown(server_sock, SHUT_RDWR);
	close(server_sock);
	return 0;
}



void reset_terminal_mode()
{
    tcsetattr(0, TCSANOW, &orig_termios);
}

void set_conio_terminal_mode()
{
    struct termios new_termios;

    /* take two copies - one for now, one for later */
    tcgetattr(0, &orig_termios);
    memcpy(&new_termios, &orig_termios, sizeof(new_termios));

    /* register cleanup handler, and set the new terminal mode */
    atexit(reset_terminal_mode);
    cfmakeraw(&new_termios);
    tcsetattr(0, TCSANOW, &new_termios);
}

void exit_handle() 
{
	// kill networking thread
	if (exit_flag_thread_listening)
	{
		pthread_cancel(message_manager);
	}
	if (exit_flag_connected)
	{
		shutdown(server_sock, SHUT_RDWR);
		close(server_sock);
	}

	// exit
    	CLEAR_SCREEN();
	printf("\n   --- Disconnected ---\n\n");
	exit(0);
}

void* message_handler(void* void_thread_idx)
{
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);
	u8  msg[DEFAULT_MSG_LEN] = {0};
	i32 ret_val = 0;
	while (1)
	{
		// skip if queue is full
		if (message_idx >= QUEUE_BUFFERS) { continue; }

		// get message
		ret_val = recv(server_sock, msg, DEFAULT_MSG_LEN, 0);
		if (ret_val < 0) { continue; }
		if (msg[0] == 0) { continue; }

		// store it
		pthread_mutex_lock(&queue_mutex);
		for (u16 i = 0; i < DEFAULT_MSG_LEN; i++)
		{
			queue[message_idx][i] = msg[i];
		}
		message_idx++;
		pthread_mutex_unlock(&queue_mutex);
	}
}

i32 keyboard_hit()
{
    struct timeval tv = { 0L, 0L };
    
	// clear and set file descriptor
	fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
	
    return select(1, &fds, NULL, NULL, &tv);
}

i8 connect_to_server(i32 argc, u8** argv)
{
	STATE = STATE_CONNECTING;

	u32 listen_port;
	if (argc < 2)
	{
		listen_port = DEFAUL_PORT;
	}
	else
	{
		i32 desired_port = atoi(argv[1]);
		if (desired_port < 0)
		{
			listen_port = (desired_port * -1);
		}
		else
		{
			listen_port = desired_port;
		}
	}

	struct sockaddr_in send_addr;
	send_addr.sin_family 		= AF_INET;
	send_addr.sin_port 			= htons(listen_port);
	send_addr.sin_addr.s_addr	= INADDR_ANY;
	socklen_t send_addr_size = sizeof(send_addr);

	ret_val       = -1;
	server_sock   = -1;

	server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_sock == -1)
	{
		printf("Failed to create socket.\r\n");
		return -1;
	}

	for (u32 i = 0; i < NUM_CONNECT_RETRIES + 2; i++)
	{
		if (i == NUM_CONNECT_RETRIES + 1)
		{
			printf("\r\nServer did not respond.\r\nAre you sure it is listening?\r\n");
			return -1;
		}
		else if (i > 0)
		{
			printf("[%u/%u] retrying connection\r\n", i, NUM_CONNECT_RETRIES);
		}

		ret_val = connect(server_sock, (struct sockaddr*) &send_addr, send_addr_size);
		if (ret_val == -1)
		{
			fflush(stdout);
			sleep(1);
		}
		else
		{
			break;
		}
	}

	return 0;
}
