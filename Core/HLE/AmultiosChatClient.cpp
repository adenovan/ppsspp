#include "Core/Core.h"
#include "Core/Host.h"
#include "AmultiosChatClient.h"
#include "i18n/i18n.h"

int chatsocket;
int chatclientstatus = CHAT_CLIENT_DISCONNECTED;
bool ChatClientRunning = false;

//later use for reconnect
const char * lastgroupname;
SceNetAdhocctlAdhocId *lastgamecode;

std::thread ChatClientThread;
bool chatScreenVisible = false;
bool updateChatScreen = false;
bool updateChatOsm = false;
int GroupNewChat = 0;
int GameNewChat = 0;
int GlobalNewChat = 0;
int newChat = 0;
int chatGuiStatus = CHAT_GUI_ALL;


ChatMessages cmList;

void ChatMessages::Add(const std::string &text, const std::string &name, int room, uint32_t namecolor) {
	std::lock_guard<std::mutex> locker(chatmutex_);
	ChatMessage chat;

	chat.name = name;
	chat.roomcolor = 0x35D8FD;
	if (name == "") {
		chat.textcolor = 0x35D8FD;
		chat.text = text;
	}
	else {
		chat.textcolor = 0xFFFFFF;
		chat.text.append(": ");
		chat.text.append(text);
	}

	chat.namecolor = namecolor;
	if (name == g_Config.sNickName.c_str()) {
		chat.namecolor = 0x3539E5;
	}

	if (name == "Lucis" || name == "tintin" || name== "adenovan") {
		chat.namecolor = 0x35D8FD;
	}

	chat.totalLength += chat.name.length();
	chat.totalLength += chat.text.length();
	if (room == CHAT_ADD_ALL) {
		chat.room = "";
		AllChatDb.push_back(chat);
	}else if (room == CHAT_ADD_GROUP || room == CHAT_ADD_ALLGROUP) {
		chat.room = "[Group]";
		chat.totalLength += chat.room.length();
		GroupChatDb.push_back(chat);
		if (room == CHAT_ADD_ALLGROUP) {
			AllChatDb.push_back(chat);
		}
	}
}


void InitChat() {

	//disable alt speed
	int iResult = 0;
	chatsocket = (int)INVALID_SOCKET;
	chatsocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (chatsocket == INVALID_SOCKET) {
		ERROR_LOG(SCENET, "Chat Client : Invalid socket");
		return;
	}
	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(30000); //27312 // Maybe read this from config too

														   // Resolve dns
	addrinfo * resultAddr;
	addrinfo * ptr;
	in_addr serverIp;
	serverIp.s_addr = INADDR_NONE;

	iResult = getaddrinfo("amultios.net", 0, NULL, &resultAddr);
	if (iResult != 0) {
		ERROR_LOG(SCENET, "Chat Client DNS Error (%s)\n", g_Config.proAdhocServer.c_str());
		host->NotifyUserMessage("DNS Error connecting to Amultios Network" + g_Config.proAdhocServer, 8.0f);
		return;
	}
	for (ptr = resultAddr; ptr != NULL; ptr = ptr->ai_next) {
		switch (ptr->ai_family) {
		case AF_INET:
			serverIp = ((sockaddr_in *)ptr->ai_addr)->sin_addr;
			break;
		}
	}

	server_addr.sin_addr = serverIp;
	iResult = connect(chatsocket, (sockaddr *)&server_addr, sizeof(server_addr));
	if (iResult == SOCKET_ERROR) {
		uint8_t * sip = (uint8_t *)&server_addr.sin_addr.s_addr;
		char buffer[512];
		snprintf(buffer, sizeof(buffer), "Failed to connect with Amultios Network error (%i) when connecting", errno);
		ERROR_LOG(SCENET, "%s", buffer);
		host->NotifyUserMessage(buffer, 8.0f);
		return;
	}

	// Prepare Login Packet
	ChatLoginPacketC2S packet;
	packet.base.opcode = OPCODE_AMULTIOS_CHAT_LOGIN;
	SceNetEtherAddr addres;
	getLocalMac(&addres);
	packet.mac = addres;
	strcpy((char *)packet.name.data, g_Config.sNickName.c_str());
	strcpy((char *)packet.pin, g_Config.sAmultiosPin.c_str());
	getServerName(packet.server);
	int sent = send(chatsocket, (char*)&packet, sizeof(packet), 0);
	changeBlockingMode(chatsocket, 1); // Change to non-blocking
	if (sent > 0) {
		I18NCategory *n = GetI18NCategory("Networking");
		host->NotifyUserMessage(n->T("Connecting to Amultios Network "), 1.0);
		if (g_Config.bEnableWlan && g_Config.bEnableNetworkChat) {
			ChatClientRunning = true;
			chatclientstatus = CHAT_CLIENT_WAITING;
			ChatClientThread = std::thread(ChatClient, 30000);
		}
		return;
	}
}

void connectChatGame(SceNetAdhocctlAdhocId *adhoc_id) {
	ChatConnectGamePacketC2S packet;
	packet.base.opcode = OPCODE_AMULTIOS_CHAT_CONNECT_GAME;
	memcpy(packet.game.data, adhoc_id->data, ADHOCCTL_ADHOCID_LEN);
	lastgamecode = adhoc_id;
	int sent = send(chatsocket, (char*)&packet, sizeof(packet), 0);
	if (sent > 0) {
		//GameChatLog.push_back("Connected to game Lobby");
	}
	else {
		cmList.Add("Failed To Join Adhoc Game", "", CHAT_ADD_ALLGROUP);
	}
}

void connectChatGroup(const char * groupname) {
	lastgroupname = groupname;
	std::string replace = createVirtualGroup(groupname);
	ChatConnectPacketC2S packet;
	const ChatGroupName * groupNameStruct = (const ChatGroupName *)replace.c_str();

	NOTICE_LOG(SCENET, "Attemp to joins virtual %s groupname on chat", replace.c_str());
	packet.base.opcode = OPCODE_AMULTIOS_CHAT_CONNECT_GROUP;
	if (groupNameStruct != NULL) packet.group = *groupNameStruct;
	int sent = send(chatsocket, (const char *)&packet, sizeof(packet), 0);

	std::string info = "";
	if (sent > 0) {
		info = "Connected to Group Lobby ";
		info += replace.c_str();
		cmList.Add(info, "", CHAT_ADD_ALLGROUP);
	}
	else {
		info = "Failed Connecting to Group Lobby ";
		info += replace.c_str();
		cmList.Add(info, "", CHAT_ADD_ALLGROUP);
	}

	if (chatScreenVisible) {
		updateChatScreen = true;
	}
	else {
		updateChatOsm = true;
	}
	return;
}

void disconnectChatGroup() {
	ChatDisconnectPacketC2S packet;
	packet.base.opcode = OPCODE_AMULTIOS_CHAT_DISCONNECT_GROUP;
	int sent = send(chatsocket, (const char *)&packet, sizeof(packet), 0);
	std::string info = "";
	std::string replace = createVirtualGroup(lastgroupname);
	info = "Disconnected from Group Lobby ";
	info += replace.c_str();
	if (sent > 0) {
		cmList.Add(info, "", CHAT_ADD_ALLGROUP);
	}

	if (chatScreenVisible) {
		updateChatScreen = true;
	}
	else {
		updateChatOsm = true;
	}

	return;
}

int ChatClient(int port) {
	int rxpos = 0;
	uint8_t rx[1024];

	// Last Ping Time
	uint64_t lastping = 0;

	// Last Time Reception got updated
	uint64_t lastreceptionupdate = 0;

	uint64_t now;

	while (ChatClientRunning) {
		// Ping Serve

		now = real_time_now()*1000000.0; 
		if (now - lastping >= PSP_ADHOCCTL_PING_TIMEOUT) { 
			lastping = now;

			// Prepare Packet
			uint8_t opcode = OPCODE_PING;
			int iResult = send(chatsocket, (const char *)&opcode, 1, 0);
		}

		int error = errno;
		// Wait for Incoming Data
		int received = recv(chatsocket, (char *)(rx + rxpos), sizeof(rx) - rxpos, 0);
		if (received == SOCKET_ERROR) {
			VERBOSE_LOG(SCENET, "Socket Error (%i) on recv", error);
		}
		// Received Data
		if (received > 0) {
			// Fix Position
			rxpos += received;

			// Log Incoming Traffic
			//printf("Received %d Bytes of Data from Server\n", received);
			INFO_LOG(SCENET, "Received %d Bytes of Data from Chat Server", received);
		}

		// Handle Packets
		if (rxpos > 0) {

			// Chat Packet
			if (rx[0] == OPCODE_CHAT) {
				INFO_LOG(SCENET, "Chat Client: Incoming OPCODE_CHAT");
				// Enough Data available
				if (rxpos >= (int)sizeof(SceNetAdhocctlChatPacketS2C)) {
					// Cast Packet
					SceNetAdhocctlChatPacketS2C * packet = (SceNetAdhocctlChatPacketS2C *)rx;
					// Add Incoming Chat to HUD GROUP AND ALL
					NOTICE_LOG(SCENET, "Received Group Chat message %s", packet->base.message);
					cmList.Add((char *)packet->base.message, (char *)packet->name.data, CHAT_ADD_ALLGROUP);
					//im new to pointer btw :( doesn't know its safe or not this should update the chat screen when data coming
					if (chatScreenVisible && (chatGuiStatus == CHAT_GUI_GROUP || chatGuiStatus == CHAT_GUI_ALL) ) {
						updateChatScreen = true;
					}
					else {
						updateChatOsm = true;
						if (GroupNewChat < 50) {
							GroupNewChat += 1;
							newChat += 1;
						}
					}
					// Move RX Buffer
					memmove(rx, rx + sizeof(SceNetAdhocctlChatPacketS2C), sizeof(rx) - sizeof(SceNetAdhocctlChatPacketS2C));

					// Fix RX Buffer Length
					rxpos -= sizeof(SceNetAdhocctlChatPacketS2C);
				}
			}

			else if (rx[0] == OPCODE_GLOBAL_CHAT) {
				INFO_LOG(SCENET, "Chat Client: Incoming OPCODE_GLOBAL_CHAT)");
				// Enough Data available
				if (rxpos >= (int)sizeof(SceNetAdhocctlChatPacketS2C)) {
					// Cast Packet
					SceNetAdhocctlChatPacketS2C * packet = (SceNetAdhocctlChatPacketS2C *)rx;
					// Add Incoming Chat to HUD ALL
					NOTICE_LOG(SCENET, "Received Global chat message %s", packet->base.message);
					cmList.Add((char *)packet->base.message, (char *)packet->name.data, CHAT_ADD_ALL);
					if (chatScreenVisible &&( chatGuiStatus == CHAT_GUI_SERVER || chatGuiStatus == CHAT_GUI_ALL)) {
						updateChatScreen = true;
					}
					else {
						updateChatOsm = true;
						if (GlobalNewChat < 50) {
							GlobalNewChat += 1;
							newChat += 1;
						}
					}
					// Move RX Buffer
					memmove(rx, rx + sizeof(SceNetAdhocctlChatPacketS2C), sizeof(rx) - sizeof(SceNetAdhocctlChatPacketS2C));

					// Fix RX Buffer Length
					rxpos -= sizeof(SceNetAdhocctlChatPacketS2C);
				}
			}

			else if (rx[0] == OPCODE_GAME_CHAT) {
				INFO_LOG(SCENET, "ChatClient: Incoming OPCODE_GAME_CHAT");
				// Enough Data available
				if (rxpos >= (int)sizeof(SceNetAdhocctlChatPacketS2C)) {
					// Cast Packet
					SceNetAdhocctlChatPacketS2C * packet = (SceNetAdhocctlChatPacketS2C *)rx;
					// Should we make more room?
					NOTICE_LOG(SCENET, "Received Game Chat message %s", packet->base.message);
					if (chatScreenVisible && chatGuiStatus == CHAT_GUI_GAME) {
						updateChatScreen = true;
					}
					else {
						updateChatOsm = true;
						if (GameNewChat < 50) {
							GameNewChat += 1;
							newChat += 1;
						}
					}
					// Move RX Buffer
					memmove(rx, rx + sizeof(SceNetAdhocctlChatPacketS2C), sizeof(rx) - sizeof(SceNetAdhocctlChatPacketS2C));

					// Fix RX Buffer Length
					rxpos -= sizeof(SceNetAdhocctlChatPacketS2C);
				}
			}

			else if (rx[0] == OPCODE_AMULTIOS_LOGIN_SUCCESS) {
				DEBUG_LOG(SCENET, "ChatClient : OPCODE_LOGIN_SUCCESS");
				if (rxpos >= (int)sizeof(SceNetAdhocctlNotifyPacketS2C)) {
					// Cast Packet
					SceNetAdhocctlNotifyPacketS2C * packet = (SceNetAdhocctlNotifyPacketS2C *)rx;
					I18NCategory *n = GetI18NCategory("Networking");
					host->NotifyUserMessage(n->T(packet->reason), 2.0);
					chatclientstatus = CHAT_CLIENT_CONNECTED;
					// Move RX Buffer
					memmove(rx, rx + sizeof(SceNetAdhocctlNotifyPacketS2C), sizeof(rx) - sizeof(SceNetAdhocctlNotifyPacketS2C));

					// Fix RX Buffer Length
					rxpos -= sizeof(SceNetAdhocctlNotifyPacketS2C);
					cmList.Add("Connected to Amultios Chat Server", "",CHAT_ADD_ALL);
					if (chatScreenVisible && (chatGuiStatus == CHAT_GUI_SERVER || chatGuiStatus == CHAT_GUI_ALL)) {
						updateChatScreen = true;
					}
					else {
						updateChatOsm = true;
					}
				}
			}

			else if (rx[0] == OPCODE_AMULTIOS_LOGIN_FAILED) {
				DEBUG_LOG(SCENET, "Chat Client: OPCODE_REJECTED");
				if (rxpos >= (int)sizeof(SceNetAdhocctlNotifyPacketS2C)) {
					// Cast Packet
					SceNetAdhocctlNotifyPacketS2C * packet = (SceNetAdhocctlNotifyPacketS2C *)rx;
					// Add Incoming Chat to HUD
					NOTICE_LOG(SCENET, "Received rejected message %s", packet->reason);
					I18NCategory *n = GetI18NCategory("Networking");
					host->NotifyUserMessage(n->T(packet->reason), 2.0);
					// Move RX Buffer
					memmove(rx, rx + sizeof(SceNetAdhocctlNotifyPacketS2C), sizeof(rx) - sizeof(SceNetAdhocctlNotifyPacketS2C));

					// Fix RX Buffer Length
					rxpos -= sizeof(SceNetAdhocctlNotifyPacketS2C);
					cmList.Add("Cannot Connect To Amultios Server Login Failed", "", CHAT_ADD_ALL);
					cmList.Add(packet->reason,"", CHAT_ADD_ALL);
					if (chatScreenVisible && (chatGuiStatus == CHAT_GUI_SERVER || chatGuiStatus == CHAT_GUI_ALL)) {
						updateChatScreen = true;
					}
					else {
						updateChatOsm = true;
					}
					break;
				}
			}
		}

		// reseted by peer end thread let gui reconnect
		if (chatclientstatus == CHAT_CLIENT_WAITING && error == ECONNRESET) {
			break;
		}

		// aborted
		if (chatclientstatus == CHAT_CLIENT_WAITING && error == ECONNABORTED) {
			break;
		}
		sleep_ms(1); // Using 1ms for faster response just like AdhocServer
		while (Core_IsStepping() && friendFinderRunning) sleep_ms(1);
	}

	chatclientstatus = CHAT_CLIENT_DISCONNECTED;
	// Log Shutdown
	INFO_LOG(SCENET, "CHAT_CLIENT: End of CHAT CLIENT Thread");
	closesocket(chatsocket);
	// Return Success
	return 0;
}

void Reconnect() {
	//reconnect
	if (chatclientstatus == CHAT_CLIENT_DISCONNECTED || chatclientstatus == CHAT_CLIENT_WAITING) {
		cmList.Add("Connection to Amultios Network Lost", "");
		cmList.Add("Reconnect in Progress...", "");
		if (chatScreenVisible) {
			updateChatScreen = true;
		}
		else {
			updateChatOsm = true;
		}

		// init only if disconnected (END OF Chat Client THREAD)
		if (chatclientstatus != CHAT_CLIENT_WAITING && chatclientstatus != CHAT_CLIENT_CONNECTED) {
			TerminateChat();
			InitChat();
			connectChatGame(lastgamecode);
			if (friendFinderRunning && lastgroupname != NULL && threadStatus == ADHOCCTL_STATE_CONNECTED) {
				connectChatGroup(lastgroupname);
			}
		}
	}
}

void sendChat(std::string chatString) {

	//check reconnect
	Reconnect();

	if (ChatClientRunning && chatclientstatus == CHAT_CLIENT_CONNECTED)
	{
		I18NCategory *n = GetI18NCategory("Networking");
		SceNetAdhocctlChatPacketC2S chat;
		switch (chatGuiStatus) {
			case CHAT_GUI_GROUP:
				chat.base.opcode = OPCODE_CHAT;
				break;
			case CHAT_GUI_GAME:
				chat.base.opcode = OPCODE_GAME_CHAT;
				break;
			case CHAT_GUI_SERVER:
				chat.base.opcode = OPCODE_GLOBAL_CHAT;
			default :
				chat.base.opcode = OPCODE_GLOBAL_CHAT;
				break;
		}

		// Send Chat to Server 
		if (!chatString.empty()) {
			//maximum char allowed is 64 character for compability with original server (pro.coldbird.net)
			std::string message = chatString.substr(0, 63);
			//Send Chat Messages
			std::string name = g_Config.sNickName.c_str();
			NOTICE_LOG(SCENET, "Send Chat %s to Adhoc Server", chat.message);
			int chatResult;
			switch (chatGuiStatus) {
			case CHAT_GUI_GROUP:
				if (friendFinderRunning) {
					strcpy(chat.message, message.c_str());
					chatResult = send(chatsocket, (const char *)&chat, sizeof(chat), 0);
					if (chatResult == SOCKET_ERROR) {
						int error = errno;
						ERROR_LOG(SCENET, "Socket error (%i) when sending", error);
					}
					else {
						cmList.Add(message, name, CHAT_ADD_ALLGROUP);
					}
				}
				else {
					cmList.Add("Group Not Available, please go to adhoc lobby or gathering hall", "", CHAT_ADD_GROUP);
				}
				break;
			/*
			case CHAT_GUI_GAME:
				if (friendFinderRunning) {
					strcpy(chat.message, message.c_str());
					chatResult = send(chatsocket, (const char *)&chat, sizeof(chat), 0);
					if (chatResult == SOCKET_ERROR) {
						int error = errno;
						ERROR_LOG(SCENET, "Socket error (%i) when sending", error);
					}
					else {
						//GameChatLog.push_back(name.substr(0, 8) + ": " + chat.message);
						//AllChatLog.push_back("[Game] " + name.substr(0, 8) + ": " + chat.message);
					}
				}
				else {
					//GameChatLog.push_back("Game Lobby Not Available, please go to adhoc lobby or gathering hall");
				}
				break;

			case CHAT_GUI_SERVER:
				strcpy(chat.message, message.c_str());
				chatResult = send(chatsocket, (const char *)&chat, sizeof(chat), 0);
				if (chatResult == SOCKET_ERROR) {
					int error = errno;
					ERROR_LOG(SCENET, "Socket error (%i) when sending", error);
				}
				else {
					AllChatLog.push_back(name.substr(0, 8) + ": " + chat.message);
				}
				break;
			*/
			case CHAT_GUI_ALL:
				bool toGroup = false;
				std::string command = message.substr(0, 6);
				if (command == "@group") {
					if (friendFinderRunning) {
						toGroup = true;
						chat.base.opcode = OPCODE_CHAT;
						message = message.erase(0, 6);
					}
					else {
						cmList.Add("Group Not Available, please go to adhoc lobby or gathering hall", "", CHAT_ADD_ALL);
						return;
					}
				}
				strcpy(chat.message, message.c_str());
				chatResult = send(chatsocket, (const char *)&chat, sizeof(chat), 0);
				if (chatResult == SOCKET_ERROR) {
					int error = errno;
					ERROR_LOG(SCENET, "Socket error (%i) when sending", error);
				}
				else {
					if (toGroup) {
						cmList.Add(message, name, CHAT_ADD_ALLGROUP);
					}
					else {
						cmList.Add(message, name, CHAT_ADD_ALL);
					}

				}
				break;
			}

			//update chatlog
			if (chatScreenVisible) {
				updateChatScreen = true;
			}
		}
	}
}

std::string createVirtualGroup(const char * groupname) {
	std::string virtgroupname = "";
	switch (g_Config.iServerChannel) {
	case 27313:
		virtgroupname += "Midgard";
		break;
	case 27314:
		virtgroupname += "Asgard";
		break;
	case 27315:
		virtgroupname += "Vanaheim";
		break;
	case 27316:
		virtgroupname += "Alfheim";
		break;
	case 27317:
		virtgroupname += "Helheim";
		break;
	default:
		virtgroupname += "PPSSPP";
		break;
	}
	char safegroupname[ADHOCCTL_GROUPNAME_LEN + 1];
	memset(safegroupname, 0, sizeof(safegroupname));
	strncpy(safegroupname, groupname, ADHOCCTL_GROUPNAME_LEN);
	virtgroupname += safegroupname;
	return virtgroupname;
}

void getServerName(char * servername) {

	switch (g_Config.iServerChannel) {
	case 27313:
		strcpy(servername, "Midgard");
		break;
	case 27314:
		strcpy(servername, "Asgard");
		break;
	case 27315:
		strcpy(servername, "Vanaheim");
		break;
	case 27316:
		strcpy(servername, "Alfheim");
		break;
	case 27317:
		strcpy(servername, "Helheim");
		break;
	default:
		strcpy(servername, "Uknown");
		break;
	}

}

void TerminateChat() {
	if (ChatClientRunning) {
		ChatClientRunning = false;
		if (ChatClientThread.joinable()) {
			ChatClientThread.join();
		}
	}
}


// used by UI

std::vector<std::string> Split(const std::string& text, const std::string& name, const std::string& group)
{	
	std::vector<std::string> ret;

	size_t spliton = size_t(40) - group.length();
	size_t firstSentenceEnd = size_t(0);

	for (auto i = 0; i<text.length(); i++) {
		if (isspace(text[i])) {
			if (i>= 10 && i <= spliton) {
				firstSentenceEnd = i+1;
			}
		}
	}

	if (firstSentenceEnd == 0 && spliton < text.length()) {
		firstSentenceEnd = spliton;
	}

	ret.push_back(text.substr(0, firstSentenceEnd));
	ret.push_back(text.substr(firstSentenceEnd));
	return ret;
}

bool isPlayer(std::string pname, std::string logname) {

	if (logname == "") return false;
	if (pname == logname) return true;
	if (logname.length() >= 16 && pname == logname.substr(8, 16)) return true;
	if (logname.length() >= 15 && pname == logname.substr(7, 15)) return true;

	return false;
}

bool isPlayerGM(std::string pname) {

	if (pname == "Lucis" || pname== "tintin" || pname == "adenovan" || pname == "Amultios") return true;
	return false;
}