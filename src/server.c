// system
#include "stdlib.h"
#include "stdio.h"
#include "time.h"

#include "unistd.h"
#include "signal.h"

#include "arpa/inet.h"
#include "sys/types.h"
#include "sys/socket.h"
#include "pthread.h"

// local
#include "types.h"
#include "common.h"


// defined constants
#define NUM_CONNECTIONS_PER_SOCK	1
#define NUM_THREADS					10

#define TIMER_OFF					0
#define TIMER_ON					1
#define TIMER_RW					2

#define NUM_DIRECTIONS				8
#define NORTH						0
#define SOUTH						1
#define WEST						2
#define EAST						3
#define NORTH_WEST					4
#define NORTH_EAST					5
#define SOUTH_WEST					6
#define SOUTH_EAST					7

#define AUTH_FAIL 					0
#define AUTH_SUCC 					1
#define AUTH_USED 					2

#define SENT 						"Sent"
#define RECV						"Received"

#define AUTH_FILE					"Authentication.txt"


// macros
#define LOCK 					pthread_mutex_lock(&print_mutex)
#define UNLOCK 					pthread_mutex_unlock(&print_mutex)

#define LOG(...)				LOCK; printf("[LOG]      " __VA_ARGS__); UNLOCK
#define WORKER(idx, ...)		LOCK; printf("[WORKER %u] ", idx); printf(__VA_ARGS__); UNLOCK
#define WARN(...)				LOCK; printf("[WARN]     " __VA_ARGS__); UNLOCK
#define ERROR(...)				LOCK; printf("[ERROR]    " __VA_ARGS__); UNLOCK; return 0			
#if DEBUG_MODE
#	define DEBUG(...)			LOCK; printf("[DEBUG]    " __VA_ARGS__); UNLOCK
#   define DEBUG_MESSAGE(x, bytes, msg)	\
	{\
		LOCK;\
		printf("[DEBUG]    " x " Message: %d bytes\n", bytes);\
		printf("----------- Begin Message -----------\n");\
		for (u16 i = 0; i < DEFAULT_MSG_LEN; i++)\
		{\
			if (msg[i] == END_OF_TRANSMISSION) { break; }\
			else\
			{\
				printf("%c", msg[i]);\
			}\
		}\
		printf("\n------------ End Message ------------\n");\
		UNLOCK;\
	}
#   define DEBUG_QUEUE() \
	{\
		LOCK;\
		printf("[DEBUG]    Socket Queue\n");\
		printf("------------ Begin ------------\n");\
		printf("batch idx   - %u\n", queue.batch_idx);\
		printf("idx         - %u\n", queue.idx);\
		printf("clients:    ");\
		for (u16 i = 0; i <= queue.batch_idx; i++)\
		{\
			if (i != 0) { printf("            "); }\
			printf("[%d] :: [", i);\
			for (u16 j = 0; j < QUEUE_CLIENT_BUFFER_LEN; j++)\
			{\
				if (queue.client_batch[i][j] == DEFAULT_SOCKET)\
					printf("-,");\
				else\
					printf("%d,", queue.client_batch[i][j]);\
			}\
			printf("]\n");\
		}\
		printf("------------- End -------------\n");\
		UNLOCK;\
	}
#else
#	define DEBUG(...)
#   define DEBUG_MESSAGE(x, bytes, msg)		
#   define DEBUG_QUEUE()
#endif	



// structs
typedef struct
{
	u8  usernames[DEFAULT_NUM_ACCOUNTS][DEFAULT_NAME_LENGTH];
	i64 seconds[DEFAULT_NUM_ACCOUNTS];
	i64 nano[DEFAULT_NUM_ACCOUNTS];
	u32 won[DEFAULT_NUM_ACCOUNTS];
	u32 played[DEFAULT_NUM_ACCOUNTS];
	u16 index;
} Leaderboard;

typedef struct
{
	u8 usernames[DEFAULT_NUM_ACCOUNTS][DEFAULT_NAME_LENGTH];
	u8 passwords[DEFAULT_NUM_ACCOUNTS][DEFAULT_NAME_LENGTH];
	u8 in_use[DEFAULT_NUM_ACCOUNTS];
} AuthDatabase;

typedef struct
{
	u16   idx;
	u8    batch_idx;
	i32*  client_batch[QUEUE_BUFFERS];
} SocketQueue;


// function prototypes
void  exit_handle();
void* client_message_handler(void* void_thread_idx);
void* idle_polling_handler();
void* time_polling_handler();

void auth_init();
u32  auth_check();

void leaderboard_init();

void queue_init();
void queue_push(i32 socket);
void queue_push(i32 socket);
i32  queue_pop();

u8 reveal_map(u8* map, u8* mine_locations, u8 game_cursor);


// globals
SocketQueue 	queue;
AuthDatabase	database;
Leaderboard		leaderboard;
i32 			listen_sock;
i32 			thread_actives[NUM_THREADS];
u8				thread_timers[NUM_THREADS];
struct timespec t0[NUM_THREADS];
struct timespec t1[NUM_THREADS];
pthread_t		idle_manager;
pthread_t		time_manager;
pthread_t 		pool[NUM_THREADS];

pthread_mutex_t queue_mutex        = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t auth_mutex         = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t print_mutex        = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t time_mutex         = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t leaderboard_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t random_mutex	   = PTHREAD_MUTEX_INITIALIZER;


i32 main(i32 argc, u8** argv)
{
	// handle signal and parse cli
	signal(SIGINT, exit_handle);
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

	// load auth database
    auth_init();

	// load leaderboard
	leaderboard_init();

	// setup listener
	struct sockaddr_in local_addr;
	local_addr.sin_family 			= AF_INET;
	local_addr.sin_port 			= htons(listen_port);
	local_addr.sin_addr.s_addr	 	= INADDR_ANY;

	listen_sock = 0;
	listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_sock == -1)
	{
		ERROR("Listener could not be created.\n");
	}

	i32 ret_val;
	ret_val = bind(listen_sock, (struct sockaddr*)&local_addr, sizeof(local_addr));
	if (ret_val == -1)
	{
		ERROR("Listener could not be bound.\n");
	}

	ret_val = listen(listen_sock, NUM_CONNECTIONS_PER_SOCK);
	if (ret_val == -1)
	{
		ERROR("Listener could not start listening.\n");
	}

	LOG("Listening on port %u\n", listen_port);

	// init queue and threads
	queue_init(&queue);
	pthread_create(&idle_manager, 0, idle_polling_handler, 0);
	pthread_create(&time_manager, 0, time_polling_handler, 0);
	u16 thread_indices[NUM_THREADS];
	for (u8 i = 0; i < NUM_THREADS; i++)
	{
		thread_indices[i] = i;
		pthread_create(&pool[i], 0, client_message_handler, &thread_indices[i]);
		LOG("Client thread created  (%u/%u)\n", i+1, NUM_THREADS);
	}

	// listener connection polling
	i32 client_sock = 0;
	struct sockaddr_storage client_addr;
	socklen_t client_addr_size = sizeof(client_addr);
	while (1) 
	{
		client_sock = accept(listen_sock, (struct sockaddr *) &client_addr, &client_addr_size);
		if (client_sock != -1)
		{
			LOG("Client connected: %d\n", client_sock);
			queue_push(client_sock);
			DEBUG_QUEUE();
		}
		else
		{
			WARN("Client connection attempted, but failed\n");
		}

		sleep(1);
	}

	WARN("\nUNEXPECTED EXIT.\n");
	exit_handle();
}

// thread handlers
void* client_message_handler(void* void_thread_idx)
{
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);
	u16 thread_idx = *((u16*) void_thread_idx);
	
	thread_actives[thread_idx] = DEFAULT_SOCKET;
	pthread_mutex_lock(&time_mutex);
	thread_timers[thread_idx] = TIMER_OFF;
	pthread_mutex_unlock(&time_mutex);

	while (1)
	{
		// various client state
		i32 ret_val;
		u8  set_index;
		u8  auth_status						= AUTH_FAIL;
		u16 authentication_id				= 0;
		u8  password[DEFAULT_NAME_LENGTH] 	= {0};
		u8  msg[DEFAULT_MSG_LEN]			= {0};
		u8*	msg_pointer 					= msg;
		#if DEBUG_MODE
			u8 username[DEFAULT_NAME_LENGTH] = "default-player";
		#else
			u8 username[DEFAULT_NAME_LENGTH] = {0};
		#endif

		// game state
		struct timespec dt;
		u8 _x, _y, _xy;
		u8 target_cursor;
		u8 mine_locations[NUM_MINES] = {0}; 
		u8 mines_left                = NUM_MINES;
		u8 game_map[NUM_TILES];
		for(u8 i = 0; i < NUM_TILES; i++)
		{
			game_map[i] = GAME_UNKNOWN;
		}

		// default message is connected
		for (u8 i = 0; i < LEN_TYPE_CON; i++)
		{
			msg[i] = MESSAGE_TYPE_CON[i];
		}
		msg[LEN_TYPE_CON] = END_OF_TRANSMISSION;

		// client aquisition
		i32 client_sock = DEFAULT_SOCKET;
		while(client_sock == DEFAULT_SOCKET)
		{
			thread_actives[thread_idx] = queue_pop();
			client_sock = thread_actives[thread_idx];
			if (client_sock != DEFAULT_SOCKET)
			{
				// tell client they are being served
				ret_val = send(client_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
				if (ret_val < 0) { break; }
				else
				{
					DEBUG_MESSAGE(SENT, ret_val, msg);
					for (u8 i = 0; i < LEN_TYPE_CON; i++) { msg[i] = 0; }
				}
			}

			sleep(1);
		}

		// client message handling
		WORKER(thread_idx, "Client attached: %d\n", client_sock);
		while (1)
		{
			ret_val = recv(client_sock, msg, DEFAULT_MSG_LEN, 0);
			if (ret_val <= 0) { break; }
			else
			{
				DEBUG_MESSAGE(RECV, ret_val, msg);

				if (parse_header(&msg_pointer, MESSAGE_TYPE_LOGIN, LEN_TYPE_LOGIN))
				{
					DEBUG("Login message detected.\n");

					// username
					if(!parse_data(&msg_pointer, 
						username, MESSAGE_DATA_USERNAME, LEN_DATA_USERNAME)) { break; }
					DEBUG("Detected username: \"%s\"\n", username);
					
					// password
					if(!parse_data(&msg_pointer, 
						password, MESSAGE_DATA_PASSWORD, LEN_DATA_PASSWORD)) { break; }
					DEBUG("Detected password: \"%s\"\n", password);

					// check database
					u32 auth_result    = auth_check(username, password);
					auth_status        = (u8)  (auth_result >> 16);
    				authentication_id  = (u16) (auth_result & 0xffff);
					if(auth_status == AUTH_FAIL) 
					{ 
						// respond - denied
						for (u16 i = 0; i < LEN_TYPE_NOP; i++)
						{
							msg[i] = MESSAGE_TYPE_NOP[i];
						}
						msg[LEN_TYPE_ACC] = END_OF_TRANSMISSION;

						// reset buffers
						for (u8 i = 0; i < DEFAULT_NAME_LENGTH; i++)
						{
							username[i] = 0;
							password[i] = 0;
						}
					}
					else if (auth_status == AUTH_SUCC)
					{
						// respond - accepted
						for (u16 i = 0; i < LEN_TYPE_ACC; i++)
						{
							msg[i] = MESSAGE_TYPE_ACC[i];
						}
						msg[LEN_TYPE_ACC] = END_OF_TRANSMISSION;
					}
					else if (auth_status == AUTH_USED)
					{
						// respond - in use
						for (u16 i = 0; i < LEN_TYPE_USED; i++)
						{
							msg[i] = MESSAGE_TYPE_USED[i];
						}
						msg[LEN_TYPE_ACC] = END_OF_TRANSMISSION;

						// reset buffers
						for (u8 i = 0; i < DEFAULT_NAME_LENGTH; i++)
						{
							username[i] = 0;
							password[i] = 0;
						}
					}

					ret_val = send(client_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
					DEBUG_MESSAGE(SENT, ret_val, msg);
				}
				else if (parse_header(&msg_pointer, MESSAGE_TYPE_START, LEN_TYPE_START))
				{
					WORKER(thread_idx, "New Game For Client: %d\n", client_sock);

					// allocate mines
					pthread_mutex_lock(&random_mutex);
					srand(DEFAULT_RANDOM_SEED);
					for (u8 i = 0; i < NUM_MINES; i++)
					{
						do 
						{
							_x  = rand() % NUM_COLS;
							_y  = rand() % NUM_ROWS;
							_xy = (_y * NUM_ROWS) + _x;
						} while (game_map[_xy] == GAME_MINE);
						game_map[_xy]     = GAME_MINE;
						mine_locations[i] = _xy;
					}
					pthread_mutex_unlock(&random_mutex);

					// clear the map - it's just there for lookups during allocation
					for (u8 i = 0; i < NUM_TILES; i++)
					{
						game_map[i] = GAME_UNKNOWN;
					}

					// start watch
					pthread_mutex_lock(&time_mutex);
					thread_timers[thread_idx] = TIMER_ON;
					pthread_mutex_unlock(&time_mutex);

					// tell client to start
					for (u8 i = 0; i < LEN_TYPE_GO; i++)
					{
						msg[i] = MESSAGE_TYPE_GO[i];
					}
					msg[LEN_TYPE_GO] = END_OF_TRANSMISSION;
					ret_val = send(client_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
					DEBUG_MESSAGE(SENT, ret_val, msg);

					// set leaderboard values - find user
					pthread_mutex_lock(&leaderboard_mutex);

					u8  found_user = 0;
					for (u16 j = 0; j < leaderboard.index; j++)
					{
						if (found_user) { break; }
						for (u8 k = 0; k < DEFAULT_NAME_LENGTH; k++)
						{
							if (leaderboard.usernames[j][k] != username[k]) { break; }
							else if (username[k] == 0)
							{
								// found user
								found_user = 1;
								leaderboard.played[j]++;
								DEBUG("Games played -> %u\n", leaderboard.played[j]);
								break;
							}
						}
					}

					// set leaderboard values - create new entry
					if(!found_user && leaderboard.index < DEFAULT_NUM_ACCOUNTS)
					{
						DEBUG("New leaderboard entry\n");
						for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
						{
							leaderboard.usernames[leaderboard.index][j] = username[j]; 
						}
						leaderboard.seconds[leaderboard.index] = 0;
						leaderboard.nano[leaderboard.index]    = 0;
						leaderboard.won[leaderboard.index]     = 0;
						leaderboard.played[leaderboard.index]  = 1;
						leaderboard.index++;
					}

					pthread_mutex_unlock(&leaderboard_mutex);
				}
				else if (parse_header(&msg_pointer, MESSAGE_TYPE_STOP, LEN_TYPE_STOP))
				{
					WORKER(thread_idx, "Abandonning Game For Client: %d\n", client_sock);

					// reset game state
					mines_left = NUM_MINES;
					for (u8 i = 0; i < NUM_MINES; i++)
					{
						mine_locations[i] = 0;
					}
					for(u8 i = 0; i < NUM_TILES; i++)
					{
						game_map[i] = GAME_UNKNOWN;
					}

					pthread_mutex_lock(&time_mutex);
					thread_timers[thread_idx] = TIMER_OFF;
					pthread_mutex_unlock(&time_mutex);
				}
				else if (parse_header(&msg_pointer, MESSAGE_TYPE_REV, LEN_TYPE_REV))
				{
					target_cursor = msg[LEN_TYPE_REV];
					if (game_map[target_cursor] > GAME_REVEAL_8)
					{
						u8 handled = 0;

						// go through all mines
						for (u8 i = 0; i < NUM_MINES; i++)
						{
							// blew yourself up on a mine
							if (target_cursor == mine_locations[i])
							{
								// transmit
								for (u16 i = 0; i < LEN_TYPE_MINE; i++)
								{
									msg[i] = MESSAGE_TYPE_MINE[i];
								}
								msg[LEN_TYPE_MINE] = END_OF_TRANSMISSION;

								ret_val = send(client_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
								DEBUG("client blown up\n");
								DEBUG_MESSAGE(SENT, ret_val, msg);

								// reset timer
								pthread_mutex_lock(&time_mutex);
								thread_timers[thread_idx] = TIMER_OFF;
								pthread_mutex_unlock(&time_mutex);

								// reset game state
								mines_left = NUM_MINES;
								for (u8 i = 0; i < NUM_MINES; i++)
								{
									mine_locations[i] = 0;
								}
								for(u8 i = 0; i < NUM_TILES; i++)
								{
									game_map[i] = GAME_UNKNOWN;
								}

								handled = 1;
								break;
							}
						}

						// if not handled the target is not a mine
						if (!handled)
						{
							// run reveal algorithm
							reveal_map(game_map, mine_locations, target_cursor);
							{ 
								#if DEBUG_MODE
								for (u8 ix = 0; ix < NUM_COLS; ix++)
								{
									for (u8 jy = 0; jy < NUM_COLS; jy++)
									{
										if (game_map[(ix * NUM_COLS) + jy] <= GAME_REVEAL_8)
										{
											printf("%u  ", game_map[(ix * NUM_COLS) + jy]);
										}
										else if (game_map[(ix * NUM_COLS) + jy] == GAME_MINE)
										{
											printf("*  ");
										}
										else if (game_map[(ix * NUM_COLS) + jy] == GAME_FLAG)
										{
											printf("F  ");
										}
										else 
										{
											printf("-  ");
										}
									}
									printf("\n");
								}
								#endif
							}

							// serialize and send
							msg_pointer = msg;
							for (u8 i = 0; i < LEN_TYPE_ADJ; i++)
							{
								*msg_pointer = MESSAGE_TYPE_ADJ[i];
								msg_pointer++;
							}
							*msg_pointer = '\n';
							msg_pointer++;
							for (u8 i = 0; i < NUM_TILES; i++)
							{
								*msg_pointer = game_map[i];
								msg_pointer++;
							}
							*msg_pointer = END_OF_TRANSMISSION;
							msg_pointer  = msg;
							ret_val      = send(client_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
						}
					}
				}
				else if (parse_header(&msg_pointer, MESSAGE_TYPE_FLAG, LEN_TYPE_FLAG))
				{
					target_cursor = msg[LEN_TYPE_FLAG];

					// check for mines
					u8 handled = 0;
					if (game_map[target_cursor] > GAME_REVEAL_8)
					{
						for (u8 i = 0; i < NUM_MINES; i++)
						{
							if (target_cursor == mine_locations[i])
							{
								handled = 1;

								// if you take a flag off a mine location
								if (game_map[target_cursor] == GAME_FLAG) 
								{ 
									mines_left++; 
									game_map[target_cursor] = GAME_UNKNOWN;
								}

								// if you put a flag on a mine location
								else
								{ 
									mines_left--; 
									game_map[target_cursor] = GAME_FLAG;
								}

								// if the client won
								if (mines_left == 0)
								{
									// set timer to not reset or increment
									pthread_mutex_lock(&time_mutex);
									thread_timers[thread_idx] = TIMER_RW;
									pthread_mutex_unlock(&time_mutex);
									time_diff(t0[thread_idx], t1[thread_idx], &dt);

									// leaderboard interaction
									pthread_mutex_lock(&leaderboard_mutex);

									// compare with previous entry
									f64 this_time, found_time;
									u8  found_user = 0;
									for (u16 j = 0; j < leaderboard.index; j++)
									{
										if (found_user) { break; }
										for (u8 k = 0; k < DEFAULT_NAME_LENGTH; k++)
										{
											if (leaderboard.usernames[j][k] != username[k]) { break; }
											else if (username[k] == 0)
											{
												// found user
												found_user = 1;
												leaderboard.won[j]++;
												DEBUG("Games won -> %u\n", leaderboard.won[j]);

												// now compare results
												this_time  = dt.tv_sec;
        										this_time += (f64)(dt.tv_nsec / NANOSECONDS);

												found_time  = leaderboard.seconds[j];
        										found_time += (f64)(leaderboard.nano[j] / NANOSECONDS);

												if (found_time == 0 || this_time < found_time)
												{
													DEBUG("Win time was better than leaderboard!\n");
													leaderboard.seconds[j]	= (i64) dt.tv_sec;
													leaderboard.nano[j]		= (i64) dt.tv_nsec;
												}
												else
												{
													DEBUG("Win time was worse than leaderboard ...\n");
												}

												break;
											}
										}
									}

									// if we didn't find them, something is definitely wrong
									// but if we do, lets sort
									if (found_user)
									{
										// a good time to sort is now, as game wins on average are infrequent
										u8  temp_username[DEFAULT_NAME_LENGTH];
										i64 temp_seconds;
										i64 temp_nano;
										u32 temp_played;
										u32 temp_won;

										#define INCREMENT_ITERATOR() \
										{\
											leaderboard.seconds[iter] = leaderboard.seconds[iter - space];\
											leaderboard.nano   [iter] = leaderboard.nano   [iter - space];\
											leaderboard.played [iter] = leaderboard.played [iter - space];\
											leaderboard.won	   [iter] = leaderboard.won    [iter - space];\
											for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)\
											{\
												leaderboard.usernames[iter][j] = leaderboard.usernames[iter - space][j];\
											}\
											iter -= space;\
										}\

										//comparators
										i32 iter;
										f64 temp_time;
										f64 iter_time;

										for (u16 space = leaderboard.index / 2; space > 0; space /= 2) 
										{ 
											DEBUG("SORTING ITERATION\n");
											for (u16 counter = space; counter < leaderboard.index; counter++) 
											{ 
												// store temp
												temp_seconds = leaderboard.seconds[counter];
												temp_nano    = leaderboard.nano[counter];
												temp_played  = leaderboard.played[counter];
												temp_won     = leaderboard.won[counter];
												for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
												{
													temp_username[j] = leaderboard.usernames[counter][j];
												}

												// calculate time
												temp_time	 = temp_seconds;
												temp_time   += (f64)(temp_nano / NANOSECONDS);
												
												// search
												iter = counter;
												while (1)
												{
													if (iter >= leaderboard.index) { break; }
													if (iter >= space)
													{
														// time comparison
														iter_time	 = leaderboard.seconds[iter - space];
														iter_time   += (f64)(leaderboard.nano[iter - space] / NANOSECONDS);
														if (iter_time < temp_time)
														{
															INCREMENT_ITERATOR();
														}
														else if (iter_time >= temp_time - EPSILON && iter_time <= temp_time + EPSILON)
														{
															// number of games comparison
															DEBUG("-- > RUNNING WINNINGS CHECK\n");
															if(leaderboard.won[iter - space] > temp_won)
															{
																DEBUG("-- > INCREMENTING FROM WINNINGS\n");
																INCREMENT_ITERATOR();
															}
															else if(leaderboard.won[iter - space] == temp_won)
															{
																// alphabetical name comparison
																DEBUG("-- > RUNNING ALPHABETICAL CHECK\n");
																u8 failed_alphabetical = 0;
																for(u8 alpha_idx = 0; alpha_idx < DEFAULT_NAME_LENGTH; alpha_idx++)
																{
																	#define iter_char	leaderboard.usernames[iter-space][alpha_idx]
																	#define temp_char	temp_username[alpha_idx]
																	
																	// empties
																	if(!iter_char && !temp_char) { break; }

																	// capital letter offsets
																	u8 iter_offset = (iter_char < 91) ? 0 : 32;
																	u8 temp_offset = (temp_char < 91) ? 0 : 32;
																	DEBUG("%c  <  %c | %u  <  %u\n", iter_char, temp_char, (iter_char - iter_offset), (temp_char - temp_offset));
																	if ((iter_char - iter_offset) < (temp_char - temp_offset)) 
																	{
																		failed_alphabetical = 1; 
																		break; 
																	}

																	#undef iter_char
																	#undef temp_char
																}

																if(!failed_alphabetical)
																{
																	DEBUG("-- > INCREMENTING FROM ALPHABETICAL\n");
																	INCREMENT_ITERATOR();
																}
																else { break; }
															}
															else { break; }
														}
														else { break; }
													}
													else { break; }
												} 

												// restore temp
												leaderboard.seconds[iter]   = temp_seconds;
												leaderboard.nano[iter]      = temp_nano;
												leaderboard.played[iter] = temp_played;
												leaderboard.won[iter]    = temp_won;
												for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
												{
													leaderboard.usernames[iter][j] = temp_username[j];
												}
											} 
										} 
									}

									#undef INCREMENT_ITERATOR

									pthread_mutex_unlock(&leaderboard_mutex);

									// set timer to reset
									pthread_mutex_lock(&time_mutex);
									thread_timers[thread_idx] = TIMER_OFF;
									pthread_mutex_unlock(&time_mutex);
								}

								// transmit
								for (u16 i = 0; i < LEN_TYPE_LEFT; i++)
								{
									msg[i] = MESSAGE_TYPE_LEFT[i];
								}
								msg[LEN_TYPE_LEFT]     = mines_left;
								msg[LEN_TYPE_LEFT + 1] = END_OF_TRANSMISSION;

								ret_val = send(client_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
								WORKER(thread_idx, "Mines left: %u\n", mines_left);
								DEBUG_MESSAGE(SENT, ret_val, msg);

								// win - cleanup
								if (mines_left == 0)
								{
									// reset game state
									mines_left = NUM_MINES;
									for (u8 i = 0; i < NUM_MINES; i++)
									{
										mine_locations[i] = 0;
									}
									for(u8 i = 0; i < NUM_TILES; i++)
									{
										game_map[i] = GAME_UNKNOWN;
									}
								}

								break;
							}
						}
					}

					// no mine -- if you take a flag off
					if (!handled && game_map[target_cursor] == GAME_FLAG) 
					{ 
						game_map[target_cursor] = GAME_UNKNOWN;
					}

					// no mine -- if you put a flag on
					else if (!handled && game_map[target_cursor] == GAME_UNKNOWN) 
					{ 
						game_map[target_cursor] = GAME_FLAG;
					}
				}
				else if (parse_header(&msg_pointer, MESSAGE_TYPE_LEAD_P, LEN_TYPE_LEAD_P))
				{
					u16 requested_page;
					requested_page  = (u16) msg[LEN_TYPE_LEAD_P] << 8;
					requested_page |= (u16) msg[LEN_TYPE_LEAD_P+1];

					// reaching outside of whats available
					pthread_mutex_lock(&leaderboard_mutex);
					if((i32)leaderboard.index - (requested_page * LEADERBOARD_ENTRIES) <= 0)
					{
						pthread_mutex_unlock(&leaderboard_mutex);
						for (u8 i = 0; i < LEN_TYPE_LEAD_E; i++)
						{
							msg[i] = MESSAGE_TYPE_LEAD_E[i];
						}
						msg[LEN_TYPE_LEAD_E] = END_OF_TRANSMISSION;
						ret_val = send(client_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
						DEBUG_MESSAGE(SENT, ret_val, msg);
					}
					else
					{
						// put header in
						msg_pointer = msg;
						for (u8 i = 0; i < LEN_TYPE_LEAD_R; i++)
						{
							*msg_pointer = MESSAGE_TYPE_LEAD_R[i];
							msg_pointer++;
						}
						*msg_pointer = '\n';
						msg_pointer++;

						// loop through page in leaderboard
						i32 init = (i32)leaderboard.index - ((requested_page + 1) * LEADERBOARD_ENTRIES);
						for (i32 i = (init < 0) ? 0 : init; i < leaderboard.index - (requested_page * LEADERBOARD_ENTRIES); i++)
						{
							// early exits
							if ((i && i >= leaderboard.index) || (!i && !leaderboard.usernames[i][0])) { break; }

							// filter to champions
							if(!leaderboard.won[i]) { continue; }

							// username
							for (u8 j = 0; j < LEN_DATA_USERNAME; j++)
							{
								*msg_pointer = MESSAGE_DATA_USERNAME[j];
								msg_pointer++;
							}
							for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
							{
								*msg_pointer = leaderboard.usernames[i][j];
								msg_pointer++;
							}
							*msg_pointer = '\n';
							msg_pointer++;

							// seconds
							*msg_pointer = leaderboard.seconds[i] >> 56; msg_pointer++;
							*msg_pointer = leaderboard.seconds[i] >> 48; msg_pointer++;
							*msg_pointer = leaderboard.seconds[i] >> 40; msg_pointer++;
							*msg_pointer = leaderboard.seconds[i] >> 32; msg_pointer++;
							*msg_pointer = leaderboard.seconds[i] >> 24; msg_pointer++;
							*msg_pointer = leaderboard.seconds[i] >> 16; msg_pointer++;
							*msg_pointer = leaderboard.seconds[i] >> 8;  msg_pointer++;
							*msg_pointer = leaderboard.seconds[i];       msg_pointer++;
							*msg_pointer = '\n';                         msg_pointer++;

							// nanoseconds
							*msg_pointer = leaderboard.nano[i] >> 56; msg_pointer++;
							*msg_pointer = leaderboard.nano[i] >> 48; msg_pointer++;
							*msg_pointer = leaderboard.nano[i] >> 40; msg_pointer++;
							*msg_pointer = leaderboard.nano[i] >> 32; msg_pointer++;
							*msg_pointer = leaderboard.nano[i] >> 24; msg_pointer++;
							*msg_pointer = leaderboard.nano[i] >> 16; msg_pointer++;
							*msg_pointer = leaderboard.nano[i] >> 8;  msg_pointer++;
							*msg_pointer = leaderboard.nano[i];		  msg_pointer++;
							*msg_pointer = '\n';					  msg_pointer++;

							// games played
							DEBUG("SENDING PLAYED: %u\n", leaderboard.played[i]);
							*msg_pointer = leaderboard.played[i] >> 24; msg_pointer++;
							*msg_pointer = leaderboard.played[i] >> 16; msg_pointer++;
							*msg_pointer = leaderboard.played[i] >> 8;  msg_pointer++;
							*msg_pointer = leaderboard.played[i];		msg_pointer++;
							*msg_pointer = '\n';					    msg_pointer++;

							// games won
							DEBUG("SENDING WON:    %u\n", leaderboard.won[i]);
							*msg_pointer = leaderboard.won[i] >> 24; msg_pointer++;
							*msg_pointer = leaderboard.won[i] >> 16; msg_pointer++;
							*msg_pointer = leaderboard.won[i] >> 8;  msg_pointer++;
							*msg_pointer = leaderboard.won[i];		 msg_pointer++;
							*msg_pointer = '\n';					 msg_pointer++;
						}

						// transmit
						pthread_mutex_unlock(&leaderboard_mutex);
						*msg_pointer = END_OF_TRANSMISSION;
						msg_pointer  = msg;
						ret_val      = send(client_sock, msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
						DEBUG_MESSAGE(SENT, ret_val, msg);
					}
				}
				else
				{
					WARN("Message header did not match any defined types\n");
					WARN("Message: \"%s\"\n", msg);
				}

				// done parsing - reset pointer
				msg_pointer = msg;
			}
		}

		// client release
		if (auth_status == AUTH_SUCC)
		{
			pthread_mutex_lock(&auth_mutex);
			database.in_use[authentication_id] = 0;
			pthread_mutex_unlock(&auth_mutex);
		}

		thread_actives[thread_idx] = DEFAULT_SOCKET;

		pthread_mutex_lock(&time_mutex);
		thread_timers[thread_idx] = TIMER_OFF;
		pthread_mutex_unlock(&time_mutex);

		close(client_sock);
		WORKER(thread_idx, "Client disconnected\n");
	}
}

void* idle_polling_handler()
{
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);
	
	i32 ret_val;
	u16 position;

	// message statics
	u8  msg[DEFAULT_MSG_LEN] = {0};
	for (u8 i = 0; i < LEN_TYPE_QUEUE; i++)
	{
		msg[i] = MESSAGE_TYPE_QUEUE[i];
	}
	msg[LEN_TYPE_QUEUE+2] = END_OF_TRANSMISSION;
	
	while (1)
	{
		for (u8 i = 0; i <= queue.batch_idx; i++)
		{
			pthread_mutex_lock(&queue_mutex);
			for (u16 j = 0; j < (QUEUE_CLIENT_BUFFER_LEN - 1); j++)
			{
				if (queue.client_batch[i][j] == DEFAULT_SOCKET) 
				{ 
					i = 0;
					break; 
				}
				else 
				{
					position = (i * QUEUE_CLIENT_BUFFER_LEN) + j;
					msg[LEN_TYPE_QUEUE]      = position >> 8;    // high
					msg[LEN_TYPE_QUEUE + 1]  = position;         // low
					ret_val = send(queue.client_batch[i][j], msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
					if (ret_val < 0)
					{
						#define q	queue

						// drop dead connection
						DEBUG("FOUND DEAD IDLE CONNECTION:  %d\n", q.client_batch[i][j]);
						DEBUG_QUEUE();
						for (u8 x = i; x <= q.batch_idx; x++)
						{
							for (u16 y = j; y < (QUEUE_CLIENT_BUFFER_LEN - 1); y++)
							{
								if (q.client_batch[x][y] == DEFAULT_SOCKET) { break; }
								q.client_batch[x][y] = q.client_batch[x][y+1];
							}
							if (x != q.batch_idx)
							{
								q.client_batch[x][(QUEUE_CLIENT_BUFFER_LEN - 1)] = q.client_batch[x+1][0];
							}
						}
						if (q.idx == 0)
						{
							free(q.client_batch[q.batch_idx]);
							q.batch_idx--;
							q.idx = QUEUE_CLIENT_BUFFER_LEN - 1;
						}
						else 
						{
							q.idx--;
						}
						DEBUG_QUEUE();

						#undef q
					}
				}
			}
			pthread_mutex_unlock(&queue_mutex);
			sleep(1);
		}
	}
}

void* time_polling_handler()
{
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);
	
	// ~75Hz - trying not to flood the client
	const struct timespec sleep_amount = {0, 133333};
	
	struct timespec dt;
	u8  set[NUM_THREADS]     = {0};
	u8  msg[DEFAULT_MSG_LEN] = {0};
	
	// default message header
	for (u8 i = 0; i < LEN_TYPE_TIME; i++)
	{
		msg[i] = MESSAGE_TYPE_TIME[i];
	}

	while (1)
	{
		for (u8 i = 0; i < NUM_THREADS; i++)
		{
			pthread_mutex_lock(&time_mutex);
			if (thread_timers[i] == TIMER_ON)
			{
				pthread_mutex_unlock(&time_mutex);

				if (!set[i])
				{
					clock_gettime(CLOCK_MONOTONIC, &t0[i]);
					set[i] = 1;
				}

				clock_gettime(CLOCK_MONOTONIC, &t1[i]);
        		time_diff(t0[i], t1[i], &dt);

				// extract and cast
				i64 dt_sec_signed    = (i64) dt.tv_sec;
				i64 dt_nano_signed   = (i64) dt.tv_nsec;

				u64 dt_sec   = (u64) dt_sec_signed; 
				u64 dt_nano  = (u64) dt_nano_signed; 
				
				// seconds
				msg[LEN_TYPE_TIME + 0] = dt_sec >> 56;
				msg[LEN_TYPE_TIME + 1] = dt_sec >> 48;
				msg[LEN_TYPE_TIME + 2] = dt_sec >> 40;
				msg[LEN_TYPE_TIME + 3] = dt_sec >> 32;
				msg[LEN_TYPE_TIME + 4] = dt_sec >> 24;
				msg[LEN_TYPE_TIME + 5] = dt_sec >> 16;
				msg[LEN_TYPE_TIME + 6] = dt_sec >> 8;
				msg[LEN_TYPE_TIME + 7] = dt_sec;

				// nanoseconds
				msg[LEN_TYPE_TIME + 8 ] = dt_nano >> 56; 
				msg[LEN_TYPE_TIME + 9 ] = dt_nano >> 48; 
				msg[LEN_TYPE_TIME + 10] = dt_nano >> 40;
				msg[LEN_TYPE_TIME + 11] = dt_nano >> 32;
				msg[LEN_TYPE_TIME + 12] = dt_nano >> 24;
				msg[LEN_TYPE_TIME + 13] = dt_nano >> 16;
				msg[LEN_TYPE_TIME + 14] = dt_nano >> 8;
				msg[LEN_TYPE_TIME + 15] = dt_nano;

				// finalize
				msg[LEN_TYPE_TIME + 16] = END_OF_TRANSMISSION;
				send(thread_actives[i], msg, DEFAULT_MSG_LEN, MSG_NOSIGNAL);
			}
			else if (thread_timers[i] == TIMER_OFF)
			{
				pthread_mutex_unlock(&time_mutex);
				set[i] = 0;
			}
			else
			{
				pthread_mutex_unlock(&time_mutex);
			}
		}
		nanosleep(&sleep_amount, 0);
	}
}

// interupt handler
void exit_handle() 
{
	printf("\n");

	DEBUG("Killing time polling manager\n");
	pthread_cancel(time_manager);

	DEBUG("Killing workers\n");
	for (u16 i = 0; i < NUM_THREADS; i++)
	{
		pthread_cancel(pool[i]);
	}

	DEBUG("Closing active connections\n");
	for (u16 i = 0; i < NUM_THREADS; i++)
	{
		shutdown(thread_actives[i], SHUT_RDWR);
		close(thread_actives[i]);
	}
	
	DEBUG("Closing listener socket\n");
	shutdown(listen_sock, SHUT_RDWR);
	close(listen_sock);

	DEBUG("Killing idle polling manager\n");
	pthread_cancel(idle_manager);

	DEBUG("Closing idle connections\n");
	for (u16 i = 0; i <= queue.batch_idx; i++)
	{
		for (u16 j = 0; j < QUEUE_CLIENT_BUFFER_LEN; j++)
		{
			if (queue.client_batch[i][j] != DEFAULT_SOCKET)
			{
				shutdown(queue.client_batch[i][j], SHUT_RDWR);
				close(queue.client_batch[i][j]);
			}
		}
		free(queue.client_batch[i]);
	}

	// exit
	LOG("Server stopped\n");	
	exit(0);
}

// queue handling
void queue_init()
{
	#define q queue

	q.idx = 0;
	q.batch_idx = 0;
	q.client_batch[q.batch_idx] = malloc(sizeof(i32) * QUEUE_CLIENT_BUFFER_LEN);
	for (u16 i = 0; i < QUEUE_CLIENT_BUFFER_LEN; i++)
	{
		q.client_batch[q.batch_idx][i] = DEFAULT_SOCKET;
	}

	#undef q
}

void queue_push(i32 socket)
{
	#define q queue

	pthread_mutex_lock(&queue_mutex);

	// if queue is full, nop
	if (q.batch_idx == (QUEUE_BUFFERS - 1) && q.idx == (QUEUE_CLIENT_BUFFER_LEN))
	{
		pthread_mutex_unlock(&queue_mutex);
		return;
	}

	// push
	q.client_batch[q.batch_idx][q.idx] = socket;
	q.idx++;
	if (q.idx == QUEUE_CLIENT_BUFFER_LEN && q.batch_idx + 1 < QUEUE_BUFFERS)
	{
		q.idx = 0;
		q.batch_idx++;
		q.client_batch[q.batch_idx] = malloc(sizeof(i32) * QUEUE_CLIENT_BUFFER_LEN);
		for (u16 i = 0; i < QUEUE_CLIENT_BUFFER_LEN; i++)
		{
			q.client_batch[q.batch_idx][i] = DEFAULT_SOCKET;
		}
	} 

	pthread_mutex_unlock(&queue_mutex);

	#undef q
}

i32 queue_pop()
{
	#define q queue

	pthread_mutex_lock(&queue_mutex);

	// if queue is empty, return default
	if (q.batch_idx == 0 && q.idx == 0)
	{
		pthread_mutex_unlock(&queue_mutex);
		return DEFAULT_SOCKET;
	}

	// pop
	i32 ret_val = q.client_batch[0][0]; 
	for (u8 i = 0; i <= q.batch_idx; i++)
	{
		for (u16 j = 0; j < (QUEUE_CLIENT_BUFFER_LEN - 1); j++)
		{
			if (q.client_batch[i][j] == DEFAULT_SOCKET) { break; }
			q.client_batch[i][j] = q.client_batch[i][j+1];
		}
		if (i != q.batch_idx)
		{
			q.client_batch[i][(QUEUE_CLIENT_BUFFER_LEN - 1)] = q.client_batch[i+1][0];
		}
	}

	q.client_batch[q.batch_idx][q.idx] = DEFAULT_SOCKET;

	// decrement + deallocate batch if needed 
	if (q.idx == 0)
	{
		free(q.client_batch[q.batch_idx]);
		q.batch_idx--;
		q.idx = QUEUE_CLIENT_BUFFER_LEN - 1;
	}
	else 
	{
		q.idx--;
	}
	
	pthread_mutex_unlock(&queue_mutex);
	return ret_val;

	#undef q
}

// authentication
void auth_init()
{
	// zero out object
	for(u16 i = 0; i < DEFAULT_NUM_ACCOUNTS; i++)
	{
		for(u16 j = 0; j < DEFAULT_NAME_LENGTH; j++)
		{
			database.usernames[i][j] = 0;
			database.passwords[i][j] = 0;
		}
		database.in_use[i] = 0;
	}

	// read file
	u8* line;
    size_t len = 0;
    ssize_t read;

    FILE* auth_file = fopen(AUTH_FILE, "r");
    if (!auth_file) { WARN("Unable to open " AUTH_FILE "\n"); return; }

	// skip first line (header names)
	getline((char**) &line, &len, auth_file);

	// parse remaining lines
	u16 username_end = 0;
	u16 counter      = 0;
    while ((read = getline((char**) &line, &len, auth_file)) != -1) 
	{
		for(u16 i = 0; i < len; i++)
		{
			// username
			if(line[i] == ' ' || line[i] == '\t')
			{
				username_end = i + 1;
				if(i != 0 && line[i - 1] != ' ' && line[i - 1] != '\t')
				{
					for (u16 j = 0; j < i; j++)
					{
						database.usernames[counter][j] = line[j];
					}
				}
			}

			// password
			if (line[i] == '\n' || line[i] == '\r' || i == len - 1)
			{
				for (u16 j = username_end; j < i; j++)
				{
					database.passwords[counter][j - username_end] = line[j];
				}
				username_end = 0;
				counter++;
				break;
			}
		}
    }

	free(line);
}

u32 auth_check(u8* username, u8* password)
{
	u32 ret = 0; 
	for (u16 i = 0; i < DEFAULT_NUM_ACCOUNTS; i++)
	{
		// skip empty accounts
		if (database.usernames[i][0] == 0) { break; } 

		for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
		{
			// check username
			if (database.usernames[i][j] != username[j]) { break; }
			else if (database.usernames[i][j] == 0) 
			{
				// check password
				for (u8 k = 0; k < DEFAULT_NAME_LENGTH; k++)
				{
					if (database.passwords[i][k] != password[k]) 
					{ 
						// a cheap way to double break here is to just
						// manually set the iterators 
						j = 0;
						i++;
						break; 
					}
					else if (database.passwords[i][k] == 0) 
					{
						pthread_mutex_lock(&auth_mutex);
						if (!database.in_use[i])
						{
							database.in_use[i] = 1;
							pthread_mutex_unlock(&auth_mutex);
							ret  = i;
    						ret |= ((u8) AUTH_SUCC) << 16;
							return ret;
						} 
						else 
						{
							pthread_mutex_unlock(&auth_mutex);
    						ret = ((u8) AUTH_USED) << 16;
							return ret;
						}
					}
				}
			}
		}
	}
	ret = ((u8) AUTH_FAIL) << 16;
	return ret;
}

// leaderboard
void leaderboard_init()
{
	// zero everything out
	leaderboard.index = 0;
	for (u16 i = 0; i < DEFAULT_NUM_ACCOUNTS; i++)
	{
		leaderboard.seconds	  [i] = 0;
		leaderboard.nano	  [i] = 0;
		leaderboard.played	  [i] = 0;
		leaderboard.won	      [i] = 0;
		for (u8 j = 0; j < DEFAULT_NAME_LENGTH; j++)
		{
			leaderboard.usernames[i][j] = 0; 
		}
	}
}

// minesweeper
u8 reveal_map(u8* map, u8* mine_locations, u8 game_cursor) 
{ 
	// make sure this tile is unknown
    if (map[game_cursor] != GAME_UNKNOWN)
	{
		return 0;
	}

	// check if this tile is a mine
	for (u8 i = 0; i < NUM_MINES; i++)
	{
		if (mine_locations[i] == game_cursor) 
		{ 
			return 1; 
		} 
	} 
  
	// set adjacency directions
	i16 cursors[NUM_DIRECTIONS];
	for (u8 i = 0; i < NUM_DIRECTIONS; i++)
	{
		cursors[i] = -1;
	}
	{
		// north
		if (game_cursor >= NUM_ROWS)
		{
			cursors[NORTH] = game_cursor - NUM_ROWS;
		}

		// south
		if (game_cursor < NUM_TILES - NUM_ROWS - 1)
		{
			cursors[SOUTH] = game_cursor + NUM_ROWS;
		}

		// east
		if ((game_cursor % NUM_COLS) > 0)
		{
			cursors[EAST] = game_cursor - 1;
		}

		// west
		if ((game_cursor % NUM_COLS) < NUM_COLS - 1)
		{
			cursors[WEST] = game_cursor + 1;
		}

		// north east
		if ((game_cursor >= NUM_ROWS) && ((game_cursor % NUM_COLS) > 0))
		{
			cursors[NORTH_EAST] = game_cursor - NUM_ROWS - 1;
		}

		// north west
		if ((game_cursor >= NUM_ROWS) && ((game_cursor % NUM_COLS) < NUM_COLS - 1))
		{
			cursors[NORTH_WEST] = game_cursor - NUM_ROWS + 1;
		}

		// south east
		if ((game_cursor < NUM_TILES - NUM_ROWS - 1) && ((game_cursor % NUM_COLS) > 0))
		{
			cursors[SOUTH_EAST] = game_cursor + NUM_ROWS - 1;
		}

		// south west
		if ((game_cursor < NUM_TILES - NUM_ROWS - 1) && ((game_cursor % NUM_COLS) < NUM_COLS - 1))
		{
			cursors[SOUTH_WEST] = game_cursor + NUM_ROWS + 1;
		}
	}

	#if DEBUG_MODE
	printf("------------------------------------------\n");
	printf("cursor      %u\n", game_cursor);
	printf("------------------------------------------\n");
	printf("NORTH		 %d\n", cursors[NORTH]);
	printf("SOUTH 		 %d\n", cursors[SOUTH]);
	printf("WEST  		 %d\n", cursors[WEST]);
	printf("EAST  		 %d\n", cursors[EAST]);
	printf("NORTH_WEST   %d\n", cursors[NORTH_WEST]);
	printf("NORTH_EAST   %d\n", cursors[NORTH_EAST]);
	printf("SOUTH_WEST   %d\n", cursors[SOUTH_WEST]);
	printf("SOUTH_EAST   %d\n", cursors[SOUTH_EAST]);
	printf("------------------------------------------\n");
	printf("mines       ");
	for (u8 i = 0; i < NUM_MINES; i++)
	{
		printf("%u, ", mine_locations[i]);
	}
	printf("\n");
	printf("------------------------------------------\n");
	#endif

	// count adjencies
    u8 count = 0;
	for (u8 i = 0; i < NUM_MINES; i++)
	{
		for (u8 j = 0; j < NUM_DIRECTIONS; j++)
		{
			// short circuit
			if (cursors[j] >= 0 && mine_locations[i] == cursors[j]) 
			{ 
				count++;
				break;
			} 
		}
	}

	#if DEBUG_MODE
	printf("counted     %u\n", count); 
	printf("------------------------------------------\n");
	#endif

	// set to map + recurse if this tile is empty
	map[game_cursor] = count;
	if (!count) 
	{
		for (u8 j = 0; j < NUM_DIRECTIONS; j++)
		{
			if (cursors[j] >= 0)
			{
				reveal_map(map, mine_locations, cursors[j]);
			}
		}
    } 

	return 0; 
} 