﻿#include "pch.h"
#include "BDS.hpp"
#include "Event.h"
#pragma warning(disable:4996)
#define api_method(name) {#name, api_##name, 1, 0}
#define api_function(name) static PyObject* api_##name(PyObject*, PyObject*args)
#define check_ret(...) if (!res) return 0;return original(__VA_ARGS__)
#define PlayerCheck(ptr)  PlayerList[(Player*)ptr]
#pragma region 全局变量
static VA _cmdqueue = 0, _ServerNetworkHandle = 0;
static Level* _level = 0;
static Scoreboard* _scoreboard;//储存计分板名称
static unsigned _formid = 1;//表单ID
static unordered_map<Event, vector<PyObject*>> PyFuncs;//Py函数
static unordered_map<Player*, bool> PlayerList;//玩家列表
static vector<pair<string, string>> Command;//注册命令
static unordered_map<string, PyObject*> ShareData;//共享数据
static int _Damage;//伤害值
static unordered_map<PyObject*, pair<unsigned, unsigned>> tick;//执行队列
static queue<function<void()>> todos;
#pragma endregion
#pragma region 函数定义
static Json::Value toJson(const string& s) {
	Json::Value j;
	Json::CharReaderBuilder rb;
	string errs;
	Json::CharReader* r(rb.newCharReader());
	bool res = r->parse(s.c_str(), s.c_str() + s.length(), &j, &errs);
	if (!res || !errs.empty()) {
		cerr << u8"[Jsoncpp]" << errs << endl;
	}
	delete r;
	return move(j);
}
static inline VA createPacket(int type) {
	VA pkt;
	SYMCALL("?createPacket@MinecraftPackets@@SA?AV?$shared_ptr@VPacket@@@std@@W4MinecraftPacketIds@@@Z",
		&pkt, type);
	return pkt;
}
static bool EventCall(Event e, PyObject* val) {
	if (PyFuncs[e].empty())
		return true;
	bool result = true;
	int nHold = PyGILState_Check();   //检测当前线程是否拥有GIL
	PyGILState_STATE gstate;
	if (!nHold)
		gstate = PyGILState_Ensure();   //如果没有GIL，则申请获取GIL
	Py_BEGIN_ALLOW_THREADS;
	Py_BLOCK_THREADS;
	for (PyObject* fn : PyFuncs[e]) {
		if (PyObject_CallFunction(fn, "O", val) == Py_False)
			result = false;
	}
	PyErr_Print();
	Py_UNBLOCK_THREADS;
	Py_END_ALLOW_THREADS;
	if (!nHold)
		PyGILState_Release(gstate);    //释放当前线程的GIL
	return result;
}
static unsigned ModalFormRequestPacket(Player* p, string str) {
	unsigned fid = _formid++;
	if (PlayerCheck(p)) {
		VA pkt = createPacket(100);
		f(unsigned, pkt + 40) = fid;
		f(string, pkt + 48) = str;
		p->sendPacket(pkt);
	}
	return fid;
}
static bool TransferPacket(Player* p, const string& address, short port) {
	if (PlayerCheck(p)) {
		VA pkt = createPacket(85);
		f(string, pkt + 40) = address;
		f(short, pkt + 72) = port;
		//Sleep(10);
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool TextPacket(Player* p, char mode, const string& msg) {
	if (PlayerCheck(p)) {
		VA pkt = createPacket(9);
		f(char, pkt + 40) = mode;
		f(string, pkt + 48) = p->getNameTag();
		f(string, pkt + 80) = msg;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool CommandRequestPacket(Player* p, const string& cmd) {
	if (PlayerCheck(p)) {
		VA pkt = createPacket(77);
		f(string, pkt + 40) = cmd;
		SYMCALL<VA>("?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVCommandRequestPacket@@@Z",
			_ServerNetworkHandle, p->getNetId(), pkt);
		//p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool BossEventPacket(Player* p, string name, float per, int eventtype) {
	if (PlayerCheck(p)) {
		VA pkt = createPacket(74);
		f(VA, pkt + 48) = f(VA, pkt + 56) = f(VA, p->getUniqueID());
		f(int, pkt + 64) = eventtype;//0显示,1更新,2隐藏,
		f(string, pkt + 72) = name;
		f(float, pkt + 104) = per;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool setDisplayObjectivePacket(Player* p, const string& title, const string& name = "name") {
	if (PlayerCheck(p)) {
		VA pkt = createPacket(107);
		f(string, pkt + 40) = "sidebar";
		f(string, pkt + 72) = name;
		f(string, pkt + 104) = title;
		f(string, pkt + 136) = "dummy";
		f(char, pkt + 168) = 0;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool SetScorePacket(Player* p, char type, const vector<ScorePacketInfo>& slot) {
	if (PlayerCheck(p)) {
		VA pkt = createPacket(108);
		f(char, pkt + 40) = type;//{set,remove}
		f(vector<ScorePacketInfo>, pkt + 48) = slot;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
#pragma endregion
#pragma region Hook
Hook(Level_tick, void, "?tick@Level@@UEAAXXZ",
	VA a1, VA a2, VA a3, VA a4) {
	original(a1, a2, a3, a4);
	size_t l = todos.size();
	if (l > 0) {
		for (int i = 0; i < l; i++) {
			todos.front()();
			todos.pop();
		}
	}
	int nHold = PyGILState_Check();   //检测当前线程是否拥有GIL
	PyGILState_STATE gstate;
	if (!nHold)
	{
		gstate = PyGILState_Ensure();   //如果没有GIL，则申请获取GIL
	}
	Py_BEGIN_ALLOW_THREADS;
	Py_BLOCK_THREADS;
	/**************************以下加入需要调用的python脚本代码  Begin***********************/
	if (tick.empty())
		return;
	for (auto& i : tick) {
		if (!i.second.first) {
			i.second.first = i.second.second;
			PyObject_CallFunction(i.first, 0);
		}
		else i.second.first--;
	}
	/**************************以下加入需要调用的python脚本代码  End***********************/
	Py_UNBLOCK_THREADS;
	Py_END_ALLOW_THREADS;
	if (!nHold)
	{
		PyGILState_Release(gstate);    //释放当前线程的GIL
	}
}
Hook(SPSCQueue, VA, "??0?$SPSCQueue@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@$0CAA@@@QEAA@_K@Z",
	VA _this) {
	_cmdqueue = original(_this);
	return _cmdqueue;
}
Hook(Level_Level, Level*, "??0Level@@QEAA@AEBV?$not_null@V?$NonOwnerPointer@VSoundPlayerInterface@@@Bedrock@@@gsl@@V?$unique_ptr@VLevelStorage@@U?$default_delete@VLevelStorage@@@std@@@std@@V?$unique_ptr@VLevelLooseFileStorage@@U?$default_delete@VLevelLooseFileStorage@@@std@@@4@AEAVIMinecraftEventing@@_NEAEAVScheduler@@AEAVStructureManager@@AEAVResourcePackManager@@AEAVIEntityRegistryOwner@@V?$unique_ptr@VBlockComponentFactory@@U?$default_delete@VBlockComponentFactory@@@std@@@4@V?$unique_ptr@VBlockDefinitionGroup@@U?$default_delete@VBlockDefinitionGroup@@@std@@@4@@Z",
	VA a1, VA a2, VA a3, VA a4, VA a5, VA a6, VA a7, VA a8, VA a9, VA a10, VA a11, VA a12, VA a13) {
	_level = original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
	return _level;
}
Hook(GameSession_, VA, "??0GameSession@@QEAA@AEAVNetworkHandler@@V?$unique_ptr@VServerNetworkHandler@@U?$default_delete@VServerNetworkHandler@@@std@@@std@@AEAVLoopbackPacketSender@@V?$unique_ptr@VNetEventCallback@@U?$default_delete@VNetEventCallback@@@std@@@3@V?$unique_ptr@VLevel@@U?$default_delete@VLevel@@@std@@@3@E@Z",
	VA a1, VA a2, VA a3, VA a4, VA a5, VA a6, VA a7) {
	_ServerNetworkHandle = f(VA, a3);
	return original(a1, a2, a3, a4, a5, a6, a7);
}
Hook(ChangeSettingCommand_setup, void, "?setup@ChangeSettingCommand@@SAXAEAVCommandRegistry@@@Z",//"?setup@ChangeSettingCommand@@SAXAEAVCommandRegistry@@@Z",?setup@KillCommand@@SAXAEAVCommandRegistry@@@Z
	VA _this) {
	for (auto& cmd : Command) {
		SYMCALL("?registerCommand@CommandRegistry@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PEBDW4CommandPermissionLevel@@UCommandFlag@@3@Z",
			_this, cmd.first.c_str(), cmd.second.c_str(), 0, 0, 0x40);
	}
	original(_this);
}
Hook(ServerScoreboard_, Scoreboard*, "??0ServerScoreboard@@QEAA@VCommandSoftEnumRegistry@@PEAVLevelStorage@@@Z",
	VA _this, VA a2, VA a3) {
	_scoreboard = (Scoreboard*)original(_this, a2, a3);
	return _scoreboard;
}
Hook(onConsoleOutput, ostream&, "??$_Insert_string@DU?$char_traits@D@std@@_K@std@@YAAEAV?$basic_ostream@DU?$char_traits@D@std@@@0@AEAV10@QEBD_K@Z",
	ostream& _this, const char* str, VA size) {
	bool res = EventCall(Event::onConsoleOutput, PyUnicode_FromString(str));
	if (!res)return _this;
	return original(_this, str, size);
}
Hook(onConsoleOutput2, int, "printf",
	const char* format, const char* str) {
	bool res = EventCall(Event::onConsoleInput, PyUnicode_FromString(str));
	check_ret(format, str);
}
Hook(onConsoleInput, bool, "??$inner_enqueue@$0A@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@?$SPSCQueue@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@$0CAA@@@AEAA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
	VA _this, const string& cmd) {
	static bool debug = false;
	if (cmd == "pydebug") {
		if (debug) {
			debug = false;
		}
		else {
			debug = true;
			cout << '>';
		}
		return 0;
	}
	if (debug) {
		PyRun_SimpleString(cmd.c_str());
		cout << '>';
		return 0;
	}
	bool res = EventCall(Event::onConsoleInput, PyUnicode_FromString(cmd.c_str()));
	check_ret(_this, cmd);
}
Hook(onPlayerJoin, void, "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVSetLocalPlayerAsInitializedPacket@@@Z",
	VA _this, VA a2, VA a3) {
	Player* p = SYMCALL<Player*>("?_getServerPlayer@ServerNetworkHandler@@AEAAPEAVServerPlayer@@AEBVNetworkIdentifier@@E@Z",
		_this, a2, *(VA*)(a3 + 16));
	PlayerList[p] = true;
	EventCall(Event::onPlyaerJoin, PyLong_FromUnsignedLongLong((VA)p));
	original(_this, a2, a3);
}
Hook(onPlayerLeft, void, "?_onPlayerLeft@ServerNetworkHandler@@AEAAXPEAVServerPlayer@@_N@Z",
	VA _this, Player* p, char v3) {
	EventCall(Event::onPlyaerLeft, PyLong_FromUnsignedLongLong((VA)p));
	PlayerList[p] = false;
	PlayerList.erase(p);
	return original(_this, p, v3);
}
Hook(onUseItem, bool, "?useItemOn@GameMode@@UEAA_NAEAVItemStack@@AEBVBlockPos@@EAEBVVec3@@PEBVBlock@@@Z",
	VA _this, ItemStack* item, BlockPos* bp, char a4, VA a5, Block* b) {
	Player* p = f(Player*, _this + 8);
	short iid = item->getId();
	short iaux = item->mAuxValue;
	string iname = item->getName();
	BlockLegacy* bl = b->getBlockLegacy();
	short bid = bl->getBlockItemID();
	string bn = bl->getBlockName();
	bool res = EventCall(Event::onUseItem, Py_BuildValue("{s:K,s:i,s:i,s:s,s:s,s:i,s:[i,i,i]}",
		"player", p,
		"itemid", iid,
		"itemaux", iaux,
		"itemname", iname.c_str(),
		"blockname", bn.c_str(),
		"blockid", bid,
		"position", bp->x, bp->y, bp->z
	));
	check_ret(_this, item, bp, a4, a5, b);
}
Hook(onPlaceBlock, bool, "?mayPlace@BlockSource@@QEAA_NAEBVBlock@@AEBVBlockPos@@EPEAVActor@@_N@Z",
	BlockSource* _this, Block* b, BlockPos* bp, unsigned __int8 a4, Actor* p, bool _bool) {
	bool res = true;
	if (PlayerCheck(p)) {
		BlockLegacy* bl = b->getBlockLegacy();
		short bid = bl->getBlockItemID();
		string bn = bl->getBlockName();
		res = EventCall(Event::onPlaceBlock, Py_BuildValue("{s:K,s:s,s:i,s:[i,i,i]}",
			"player", p,
			"blockname", bn.c_str(),
			"blockid", bid,
			"position", bp->x, bp->y, bp->z
		));
	}
	check_ret(_this, b, bp, a4, p, _bool);
}
Hook(onDestroyBlock, bool, "?_destroyBlockInternal@GameMode@@AEAA_NAEBVBlockPos@@E@Z",
	VA _this, BlockPos* bp) {
	Player* p = f(Player*, _this + 8);
	BlockSource* bs = p->getBlockSource();
	Block* b = bs->getBlock(*bp);
	BlockLegacy* bl = b->getBlockLegacy();
	short bid = bl->getBlockItemID();
	string bn = bl->getBlockName();
	bool res = true;
	res = EventCall(Event::onDestroyBlock, Py_BuildValue("{s:K,s:s,s:i,s:[i,i,i]}",
		"player", p,
		"blockname", bn.c_str(),
		"blockid", bid,
		"position", bp->x, bp->y, bp->z
	));
	check_ret(_this, bp);
}
Hook(onOpenChest, bool, "?use@ChestBlock@@UEBA_NAEAVPlayer@@AEBVBlockPos@@E@Z",
	VA _this, Player* p, BlockPos* bp) {
	bool res = EventCall(Event::onOpenChest, Py_BuildValue("{s:K,s:[i,i,i]}",
		"player", p,
		"position", bp->x, bp->y, bp->z
	));
	check_ret(_this, p, bp);
}
Hook(onOpenBarrel, bool, "?use@BarrelBlock@@UEBA_NAEAVPlayer@@AEBVBlockPos@@E@Z",
	VA _this, Player* p, BlockPos* bp) {
	bool res = EventCall(Event::onOpenBarrel, Py_BuildValue("{s:K,s:[i,i,i]}",
		"player", p,
		"position", bp->x, bp->y, bp->z
	));
	check_ret(_this, p, bp);
}
Hook(onCloseChest, void, "?stopOpen@ChestBlockActor@@UEAAXAEAVPlayer@@@Z",
	VA _this, Player* p) {
	auto bp = (BlockPos*)(_this - 204);
	EventCall(Event::onCloseChest, Py_BuildValue("{s:K,s:[i,i,i]}",
		"player", p,
		"position", bp->x, bp->y, bp->z
	));
	original(_this, p);
}
Hook(onCloseBarrel, void, "?stopOpen@BarrelBlockActor@@UEAAXAEAVPlayer@@@Z",
	VA _this, Player* p) {
	auto bp = (BlockPos*)(_this - 204);
	EventCall(Event::onCloseBarrel, Py_BuildValue("{s:K,s:[i,i,i]}",
		"player", p,
		"position", bp->x, bp->y, bp->z
	));
	original(_this, p);
}
Hook(onContainerChange, void, "?containerContentChanged@LevelContainerModel@@UEAAXH@Z",
	VA a1, VA slot) {
	VA v3 = f(VA, a1 + 208);// IDA LevelContainerModel::_getContainer line 15 25
	BlockSource* bs = f(BlockSource*, f(VA, v3 + 848) + 88);
	BlockPos* bp = (BlockPos*)(a1 + 216);
	BlockLegacy* bl = bs->getBlock(*bp)->getBlockLegacy();
	short bid = bl->getBlockItemID();
	if (bid == 54 || bid == 130 || bid == 146 || bid == -203 || bid == 205 || bid == 218) {	// 非箱子、桶、潜影盒的情况不作处理
		VA v5 = (*(VA(**)(VA))(*(VA*)a1 + 160))(a1);
		if (v5) {
			ItemStack* i = (ItemStack*)(*(VA(**)(VA, VA))(*(VA*)v5 + 40))(v5, slot);
			EventCall(Event::onContainerChange, Py_BuildValue("{s:K,s:s,s:i,s:[i,i,i],s:i,s:i,s:s,s:i,s:i}",
				"player", f(Player*, a1 + 208),
				"blockname", bl->getBlockName().c_str(),
				"blockid", bid,
				"position", bp->x, bp->y, bp->z,
				"itemid", i->getId(),
				"itemcount", i->mCount,
				"itemname", i->getName().c_str(),
				"itemaux", i->mAuxValue,
				"slot", slot
			));
		}
	}
	original(a1, slot);
}
Hook(onAttack, bool, "?attack@Player@@UEAA_NAEAVActor@@@Z",
	Player* p, Actor* a) {
	bool res = EventCall(Event::onPlayerAttack, Py_BuildValue("{s:K,s:K}",
		"player", p,
		"actor", a
	));
	check_ret(p, a);
}
Hook(onChangeDimension, bool, "?_playerChangeDimension@Level@@AEAA_NPEAVPlayer@@AEAVChangeDimensionRequest@@@Z",
	VA _this, Player* p, VA req) {
	bool result = original(_this, p, req);
	if (result) {
		EventCall(Event::onChangeDimension, PyLong_FromUnsignedLongLong((VA)p));
	}
	return result;
}
Hook(onMobDie, void, "?die@Mob@@UEAAXAEBVActorDamageSource@@@Z",
	Mob* _this, VA dmsg) {
	char v72;
	Actor* sa = f(Level*, _this + 856)->fetchEntity(*(VA*)((*(VA(__fastcall**)(VA, char*))(*(VA*)dmsg + 64))(dmsg, &v72)));
	bool res = EventCall(Event::onMobDie, Py_BuildValue("{s:I,s:K,s:K}",
		"dmcase", f(unsigned, dmsg + 8),
		"actor1", _this,
		"actor2", sa//可能为0
	));
	if (res) original(_this, dmsg);
}
Hook(onMobHurt, bool, "?_hurt@Mob@@MEAA_NAEBVActorDamageSource@@H_N1@Z",
	Mob* _this, VA dmsg, int a3, bool a4, bool a5) {
	_Damage = a3;//将生物受伤的值设置为可调整
	char v72;
	Actor* sa = f(Level*, _this + 856)->fetchEntity(*(VA*)((*(VA(__fastcall**)(VA, char*))(*(VA*)dmsg + 64))(dmsg, &v72)));
	bool res = EventCall(Event::onMobHurt, Py_BuildValue("{s:i,s:K,s:K,s:i}",
		"dmcase", f(unsigned, dmsg + 8),
		"actor1", _this,
		"actor2", sa,//可能为0
		"damage", a3
	));
	check_ret(_this, dmsg, _Damage, a4, a5);
}
Hook(onRespawn, void, "?respawn@Player@@UEAAXXZ",
	Player* p) {
	EventCall(Event::onRespawn, PyLong_FromUnsignedLongLong((VA)p));
	original(p);
}
Hook(onChat, void, "?fireEventPlayerMessage@MinecraftEventing@@AEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@000@Z",
	VA _this, string& sender, string& target, string& msg, string& style) {
	EventCall(Event::onChat, Py_BuildValue("{s:s,s:s,s:s,s:s}",
		"sender", sender.c_str(),
		"target", target.c_str(),
		"msg", msg.c_str(),
		"style", style.c_str()
	));
	original(_this, sender, target, msg, style);
}
Hook(onInputText, void, "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVTextPacket@@@Z",
	VA _this, VA id, /*(TextPacket*)*/VA pkt) {
	Player* p = SYMCALL<Player*>("?_getServerPlayer@ServerNetworkHandler@@AEAAPEAVServerPlayer@@AEBVNetworkIdentifier@@E@Z",
		_this, id, f(char, pkt + 16));
	if (p) {
		string msg = f(string, pkt + 80);
		bool res = EventCall(Event::onInputText, Py_BuildValue("{s:K,s:s}",
			"player", p,
			"msg", msg.c_str()
		));
		if (res)original(_this, id, pkt);
	}
}
Hook(onInputCommand, void, "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVCommandRequestPacket@@@Z",
	VA _this, VA id, /*(CommandRequestPacket*)*/VA pkt) {
	Player* p = SYMCALL<Player*>("?_getServerPlayer@ServerNetworkHandler@@AEAAPEAVServerPlayer@@AEBVNetworkIdentifier@@E@Z",
		_this, id, f(char, pkt + 16));
	if (p) {
		string cmd = f(string, pkt + 40);
		bool res = EventCall(Event::onInputCommand, Py_BuildValue("{s:K,s:s}",
			"player", p,
			"cmd", cmd.c_str()
		));
		if (res)original(_this, id, pkt);
	}
}
Hook(onSelectForm, void, "?handle@?$PacketHandlerDispatcherInstance@VModalFormResponsePacket@@$0A@@@UEBAXAEBVNetworkIdentifier@@AEAVNetEventCallback@@AEAV?$shared_ptr@VPacket@@@std@@@Z",
	VA _this, VA id, VA handle,/*(ModalFormResponsePacket**)*/VA* ppkt) {
	VA pkt = *ppkt;
	Player* p = SYMCALL<Player*>("?_getServerPlayer@ServerNetworkHandler@@AEAAPEAVServerPlayer@@AEBVNetworkIdentifier@@E@Z",
		handle, id, f(char, pkt + 16));
	if (PlayerCheck(p)) {
		unsigned fid = f(unsigned, pkt + 40);
		string data = f(string, pkt + 48);
		if (data.back() == '\n')data.pop_back();
		EventCall(Event::onSelectForm, Py_BuildValue("{s:K,s:s,s:i}",
			"player", p,
			"selected", data.c_str(),
			"formid", fid
		));
	}
	original(_this, id, handle, ppkt);
}
Hook(onCommandBlockUpdate, void, "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVCommandBlockUpdatePacket@@@Z",
	VA _this, VA id, /*(CommandBlockUpdatePacket*)*/VA pkt) {
	bool res = true;
	Player* p = SYMCALL<Player*>("?_getServerPlayer@ServerNetworkHandler@@AEAAPEAVServerPlayer@@AEBVNetworkIdentifier@@E@Z",
		_this, id, f(char, pkt + 16));
	if (PlayerCheck(p)) {
		auto bp = f(BlockPos, pkt + 40);
		auto mode = f(unsigned short, pkt + 52);
		auto condition = f(bool, pkt + 54);
		auto redstone = f(bool, pkt + 55);
		auto cmd = f(string, pkt + 64);
		auto output = f(string, pkt + 96);
		auto rawname = f(string, pkt + 128);
		auto delay = f(int, pkt + 160);
		res = EventCall(Event::onCommandBlockUpdate, Py_BuildValue("{s:K,s:i,s:i,s:i,s:s,s:s,s:s,s:i,s:[i,i,i]}",
			"player", p,
			"mode", mode,
			"condition", condition,
			"redstone", redstone,
			"cmd", cmd.c_str(),
			"output", output.c_str(),
			"rawname", rawname.c_str(),
			"delay", delay,
			"position", bp.x, bp.y, bp.z
		));
	}
	if (res)original(_this, id, pkt);
}
Hook(onLevelExplode, bool, "?explode@Level@@QEAAXAEAVBlockSource@@PEAVActor@@AEBVVec3@@M_N3M3@Z",
	Level* _this, BlockSource* bs, Actor* a3, Vec3 pos, float a5, bool a6, bool a7, float a8, bool a9) {
	bool res = EventCall(Event::onLevelExplode, Py_BuildValue("{s:K,s:[f,f,f],s:i,s:i}",
		"actor", a3,
		"position", pos.x, pos.y, pos.z,
		"dimensionid", bs->getDimensionId(),
		"power", a5
	));
	check_ret(_this, bs, a3, pos, a5, a6, a7, a8, a9);
}
Hook(onCommandBlockPerform, bool, "?performCommand@CommandBlockActor@@QEAA_NAEAVBlockSource@@@Z",
	VA _this, BlockSource* a2) {
	//脉冲:0,重复:1,链:2
	int mode = SYMCALL<int>("?getMode@CommandBlockActor@@QEBA?AW4CommandBlockMode@@AEAVBlockSource@@@Z",
		_this, a2);
	//无条件:0,有条件:1
	bool condition = SYMCALL<bool>("?getConditionalMode@CommandBlockActor@@QEBA_NAEAVBlockSource@@@Z",
		_this, a2);
	string cmd = f(string, _this + 264);
	string rawname = f(string, _this + 296);
	BlockPos bp = f(BlockPos, _this + 44);
	bool res = EventCall(Event::onCommandBlockPerform, Py_BuildValue("{s:s,s:s,s:[i,i,i],s:i,s:i}",
		"cmd", cmd.c_str(),
		"rawname", rawname.c_str(),
		"position", bp.x, bp.y, bp.z,
		"mode", mode,
		"condition", condition
	));
	check_ret(_this, a2);
}
Hook(onMove, void, "??0MovePlayerPacket@@QEAA@AEAVPlayer@@W4PositionMode@1@HH@Z",
	VA _this, Player* p, char a3, int a4, int a5) {
	EventCall(Event::onMove, PyLong_FromUnsignedLongLong((VA)p));
	original(_this, p, a3, a4, a5);
}
Hook(onSetArmor, void, "?setArmor@Player@@UEAAXW4ArmorSlot@@AEBVItemStack@@@Z",
	Player* p, unsigned slot, ItemStack* i) {
	if (!i->getId())return original(p, slot, i);
	bool res = EventCall(Event::onSetArmor, Py_BuildValue("{s:K,s:i,s:i,s:s,s:i,s:i}",
		"player", p,
		"itemid", i->getId(),
		"itemcount", i->mCount,
		"itemname", i->getName().c_str(),
		"itemaux", i->mAuxValue,
		"slot", slot
	));
	if (res)original(p, slot, i);
}
Hook(onScoreChanged, void, "?onScoreChanged@ServerScoreboard@@UEAAXAEBUScoreboardId@@AEBVObjective@@@Z",
	const Scoreboard* _this, ScoreboardId* a2, Objective* a3) {
	/*
	原命令：
	创建计分板时：/scoreboard objectives <add|remove> <objectivename> dummy <objectivedisplayname>
	修改计分板时（此函数hook此处)：/scoreboard players <add|remove|set> <playersname> <objectivename> <playersnum>
	*/
	int scoreboardid = a2->id;
	EventCall(Event::onScoreChanged, Py_BuildValue("{s:i,s:i,s:s,s:s}",
		"scoreboardid", scoreboardid,
		"playersnum", a3->getPlayerScore(a2)->getCount(),
		"objectivename", a3->getScoreName().c_str(),
		"objectivedisname", a3->getScoreDisplayName().c_str()
	));
	original(_this, a2, a3);
}
Hook(onFallBlockTransform, void, "?transformOnFall@FarmBlock@@UEBAXAEAVBlockSource@@AEBVBlockPos@@PEAVActor@@M@Z",
	VA _this, BlockSource* a1, BlockPos* a2, Player* p, VA a4) {
	bool res = true;
	if (PlayerCheck(p)) {
		res = EventCall(Event::onFallBlockTransform, Py_BuildValue("{s:K,s:[i,i,i],s:i}",
			"player", p,
			"position", a2->x, a2->y, a2->z,
			"dimensionid", a1->getDimensionId()
		));
	}
	if (res)original(_this, a1, a2, p, a4);
}
Hook(onUseRespawnAnchorBlock, bool, "?trySetSpawn@RespawnAnchorBlock@@CA_NAEAVPlayer@@AEBVBlockPos@@AEAVBlockSource@@AEAVLevel@@@Z",
	Player* p, BlockPos* a2, BlockSource* a3, void* a4) {
	bool res = true;
	if (PlayerCheck(p)) {
		res = EventCall(Event::onUseRespawnAnchorBlock, Py_BuildValue("{s:K,s:[i,i,i],s:i}",
			"player", p,
			"position", a2->x, a2->y, a2->z,
			"dimensionid", a3->getDimensionId()
		));
	}
	check_ret(p, a2, a3, a4);
}
#pragma endregion
#pragma region API函数
// 获取版本
api_function(getVersion) {
	if (PyArg_ParseTuple(args, ":getVersion")) {
		return PyLong_FromLong(103);
	}
	return Py_False;
}
// 指令输出
api_function(logout) {
	const char* msg;
	if (PyArg_ParseTuple(args, "s:logout", &msg)) {
		SYMCALL<ostream&>("??$_Insert_string@DU?$char_traits@D@std@@_K@std@@YAAEAV?$basic_ostream@DU?$char_traits@D@std@@@0@AEAV10@QEBD_K@Z",
			&cout, msg, strlen(msg));
		return Py_True;
	}
	return Py_False;
}
// 执行指令
api_function(runcmd) {
	const char* cmd;
	if (PyArg_ParseTuple(args, "s:runcmd", &cmd)) {
		onConsoleInput::original(_cmdqueue, cmd);
		return Py_True;
	}
	return Py_False;
}
// 计时器
api_function(setTimer) {
	unsigned time; PyObject* func;
	if (PyArg_ParseTuple(args, "OI:setTimer", &func, &time)) {
		if (!PyCallable_Check(func))
			return Py_False;
		tick[func] = { time,time };
	}
	return Py_True;
}
api_function(removeTimer) {
	PyObject* func;
	if (PyArg_ParseTuple(args, "O:removeTimer", &func, &time)) {
		tick.erase(func);
	}
	return Py_True;
}
// 设置监听
api_function(setListener) {
	const char* name; PyObject* fn;
	if (PyArg_ParseTuple(args, "sO:setListener", &name, &fn)) {
		char e = toEvent(name);
		if (e != -1) {
			PyFuncs[(Event)e].push_back(fn);
			return Py_True;
		}
		else fprintf(stderr,u8"无效的监听:%d", e );
	}
	return Py_False;
}
// 共享数据
api_function(setShareData) {
	const char* index; PyObject* data;
	if (PyArg_ParseTuple(args, "sO:setShareData", &index, &data)) {
		ShareData[index] = data;
		return Py_True;
	}
	return Py_False;
}
api_function(getShareData) {
	const char* index;
	if (PyArg_ParseTuple(args, "s:getShareData", &index)) {
		return ShareData[index];
	}
	return Py_False;
}
// 设置指令说明
api_function(setCommandDescription) {
	const char* cmd, * des;
	if (PyArg_ParseTuple(args, "ss:setCommandDescribe", &cmd, &des)) {
		Command.push_back({ cmd,des });
		return Py_True;
	}
	return Py_False;
}
api_function(getPlayerList) {
	auto list = PyList_New(0);
	PyArg_ParseTuple(args, ":getPlayerList");
	for (auto& p : PlayerList) {
		if (p.second)
			PyList_Append(list, PyLong_FromUnsignedLongLong((VA)p.first));
	}
	return list;
}
// 发送表单
api_function(sendCustomForm) {
	Player* p; const char* str;
	if (PyArg_ParseTuple(args, "Ks:sendCustomForm", &p, &str)) {
		return PyLong_FromLong(ModalFormRequestPacket(p, str));
	}
	return PyLong_FromLong(0);
}
api_function(sendSimpleForm) {
	Player* p; const char* title, * content, * buttons;
	if (PyArg_ParseTuple(args, "Ksss:sendSimpleForm", &p, &title, &content, &buttons)) {
		char str[0x400];
		sprintf(str, R"({"title":"%s","content":"%s","buttons":%s,"type":"form"})", title, content, buttons);
		return PyLong_FromLong(ModalFormRequestPacket(p, str));
	}
	return Py_False;
}
api_function(sendModalForm) {
	Player* p; const char* title, * content, * button1, * button2;
	if (PyArg_ParseTuple(args, "Kssss:sendModalForm", &p, &title, &content, &button1, &button2)) {
		char str[0x400];
		sprintf(str, R"({"title":"%s","content":"%s","button1":"%s","button2":"%s","type":"modal"})", title, content, button1, button2);
		return PyLong_FromLong(ModalFormRequestPacket(p, str));
	}
	return Py_False;
}
// 跨服传送
api_function(transferServer) {
	Player* p; const char* address; int port;
	if (PyArg_ParseTuple(args, "Ksi:transferServer", &p, &address, &port)) {
		if (TransferPacket(p, address, port))
			return Py_True;
	}
	return Py_False;
}
// 获取玩家信息
api_function(getPlayerInfo) {
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerInfo", &p)) {
		if (PlayerCheck(p)) {
			Vec3* pp = p->getPos();
			return Py_BuildValue("{s:s,s:[f,f,f],s:i,s:b,s:i,s:i,s:s,s:s}",
				"playername", p->getNameTag().c_str(),
				"XYZ", pp->x, pp->y, pp->z,
				"dimensionid", p->getDimensionId(),
				"isstand", p->isStand(),
				"health", p->getHealth(),
				"maxhealth", p->getMaxHealth(),
				"xuid", p->getXuid().c_str(),
				"uuid", p->getUuid().c_str()
			);
		}
	}
	return PyDict_New();
}
api_function(getActorInfo) {
	Actor* a;
	if (PyArg_ParseTuple(args, "K:getActorInfo", &a)) {
		if (!a)
			return Py_False;
		Vec3* pp = a->getPos();
		return Py_BuildValue("{s:s,s:i,s:[f,f,f],s:i,s:b,s:i,s:i}",
			"entityname", a->getEntityTypeName().c_str(),
			"entityid", a->getEntityTypeId(),
			"XYZ", pp->x, pp->y, pp->z,
			"dimensionid", a->getDimensionId(),
			"isstand", a->isStand(),
			"health", a->getHealth(),
			"maxhealth", a->getMaxHealth()
		);
	}
	return Py_False;
}
api_function(getActorInfoEx) {
	Actor* a;
	if (PyArg_ParseTuple(args, "K:getActorInfoEx", &a)) {
		if (!a)
			return Py_False;
		Tag* t = a->save();
		string info = toJson(t).toStyledString();
		t->deCompound();
		delete t;
		return PyUnicode_FromString(info.c_str());
	}
	return Py_False;
}
// 玩家权限
api_function(getPlayerPerm) {
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerPerm", &p)) {
		if (PlayerCheck(p)) {
			return PyLong_FromLong(p->getPermission());
		}
	}
	return Py_False;
}
api_function(setPlayerPerm) {
	Player* p; unsigned char lv;
	if (PyArg_ParseTuple(args, "Kb:setPlayerPerm", &p, &lv)) {
		if (PlayerCheck(p)) {
			p->setPermissionLevel(lv);
			return Py_True;
		}
	}
	return Py_False;
}
// 增加玩家等级
api_function(addLevel) {
	Player* p; int lv;
	if (PyArg_ParseTuple(args, "Ki::addLevel", &p, &lv)) {
		if (PlayerCheck(p)) {
			SYMCALL("?addLevels@Player@@UEAAXH@Z", p, lv);
			return Py_True;
		}
	}
	return Py_False;
}
// 设置玩家名字
api_function(setName) {
	Player* p; const char* name;
	if (PyArg_ParseTuple(args, "Ks:setName", &p, &name)) {
		if (PlayerCheck(p)) {
			p->setName(name);
			return Py_True;
		}
	}
	return Py_False;
}
// 玩家分数
api_function(getPlayerScore) {
	Player* p; const char* obj;
	if (PyArg_ParseTuple(args, "Ks:getPlayerScore", &p, &obj)) {
		if (PlayerCheck(p)) {
			Objective* testobj = _scoreboard->getObjective(obj);
			if (testobj) {
				auto id = _scoreboard->getScoreboardId(p);
				auto score = testobj->getPlayerScore(id);
				return PyLong_FromLong(score->getCount());
			}
			else cout << "bad objective: " << obj << endl;
		}
	}
	return Py_False;
}
api_function(modifyPlayerScore) {
	Player* p; const char* obj; int count; int mode;
	if (PyArg_ParseTuple(args, "Ksii:modifyPlayerScore", &p, &obj, &count, &mode)) {
		if (PlayerCheck(p)) {
			Objective* testobj = _scoreboard->getObjective(obj);
			if (testobj) {
				//mode:{set,add,remove}
				_scoreboard->modifyPlayerScore((ScoreboardId*)_scoreboard->getScoreboardId(p), testobj, count, mode);
				return Py_True;
			}
			else cout << "bad objective: " << obj << endl;
		}
	}
	return Py_False;
}
// 模拟玩家发送文本
api_function(talkAs) {
	Player* p; const char* msg;
	if (PyArg_ParseTuple(args, "Ks:talkAs", &p, &msg)) {
		if (TextPacket(p, 1, msg))
			return Py_True;
	}
	return Py_False;
}
// 模拟玩家执行指令
api_function(runcmdAs) {
	Player* p; const char* cmd;
	if (PyArg_ParseTuple(args, "Ks:runcmdAs", &p, &cmd)) {
		if (CommandRequestPacket(p, cmd))
			return Py_True;
	}
	return Py_False;
}
// 传送玩家
api_function(teleport) {
	Player* p; float x, y, z; int did;
	if (PyArg_ParseTuple(args, "Kfffi:teleport", &p, &x, &y, &z, &did)) {
		p->teleport({ x,y,z }, did);
		return Py_True;
	}
	return Py_False;
}
// 原始输出
api_function(tellraw) {
	const char* msg;
	Player* p;
	if (PyArg_ParseTuple(args, "Ks:tellraw", &p, &msg)) {
		if (TextPacket(p, 0, msg))
			return Py_True;
	}
	return Py_False;
}
api_function(tellrawEx) {
	const char* msg;
	Player* p; int mode;
	if (PyArg_ParseTuple(args, "Ksi:tellraw", &p, &msg, &mode)) {
		if (TextPacket(p, mode, msg))
			return Py_True;
	}
	return Py_False;
}
// boss栏
api_function(setBossBar) {
	Player* p; const char* name; float per;
	if (PyArg_ParseTuple(args, "Ksf:", &p, &name, &per)) {
		if (BossEventPacket(p, name, per, 0))
			return Py_True;
	}
	return Py_False;
}
api_function(removeBossBar) {
	Player* p;
	if (PyArg_ParseTuple(args, "K:removeBossBar", &p)) {
		if (BossEventPacket(p, "", 0.0, 2))
			return Py_True;
	}
	return Py_False;
}
// 通过玩家指针获取计分板id
api_function(getScoreBoardId) {
	Player* p;
	if (PyArg_ParseTuple(args, "K:getScoreBoardId", &p)) {
		if (PlayerCheck(p)) {
			return Py_BuildValue("{s:i}",
				"scoreboardid", _scoreboard->getScoreboardId(p)
			);
		}
	}
	return Py_False;
}
api_function(createScoreBoardId) {
	Player* p;
	if (PyArg_ParseTuple(args, "K:createScoreBoardId", &p)) {
		if (PlayerCheck(p)) {
			_scoreboard->createScoreBoardId(p);
			return Py_True;
		}
	}
	return Py_False;
}
// 修改生物受伤的伤害值!
api_function(setDamage) {
	int a;
	if (PyArg_ParseTuple(args, "i:setDamage", &a)) {
		_Damage = a;
		return Py_True;
	}
	return Py_False;
}
api_function(setServerMotd) {
	const char* n;
	if (PyArg_ParseTuple(args, "s:setServerMotd", &n) && _ServerNetworkHandle) {
		string name = n;
		SYMCALL<VA>("?allowIncomingConnections@ServerNetworkHandler@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z",
			_ServerNetworkHandle, &name, true);
		return Py_True;
	}
	return Py_False;
}
// 玩家背包相关操作
api_function(getPlayerItems) {
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerItems", &p)) {
		if (PlayerCheck(p)) {
			Json::Value j;
			for (auto& i : p->getContainer()->getSlots()) {
				j.append(toJson(i->save()));
			}
			return PyUnicode_FromString(j.toStyledString().c_str());
		}
	}
	return Py_False;
}
api_function(getPlayerHand) {
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerHand", &p)) {
		if (PlayerCheck(p)) {
			ItemStack* item = p->getSelectedItem();
			return PyUnicode_FromString(toJson(item->save()).toStyledString().c_str());
		}
	}
	return Py_False;
}
api_function(getPlayerItem) {
	Player* p; int slot;
	if (PyArg_ParseTuple(args, "Ki:getPlayerItem", &p, &slot)) {
		if (PlayerCheck(p)) {
			ItemStack* item = p->getInventoryItem(slot);
			return PyUnicode_FromString(toJson(item->save()).toStyledString().c_str());
		}
	}
	return Py_False;
}
api_function(setPlayerHand) {
	Player* p;
	const char* x;
	if (PyArg_ParseTuple(args, "Ks:setPlayerHand", &p, &x)) {
		if (PlayerCheck(p)) {
			Json::Value j = toJson(x);
			Tag* t = toTag(j);
			p->getSelectedItem()->fromTag(t);
			t->deCompound();
			p->sendInventroy();
			delete t;
			return Py_True;
		}
	}
	return Py_False;
}
api_function(setPlayerItems) {
	Player* p;
	const char* x;
	if (PyArg_ParseTuple(args, "Ks:setPlayerItems", &p, &x)) {
		if (PlayerCheck(p)) {
			Json::Value j = toJson(x);
			if (j.isArray()) {
				vector<ItemStack*> is = p->getContainer()->getSlots();
				for (unsigned i = 0; i < j.size(); i++) {
					Tag* t = toTag(j[i]);
					is[i]->fromTag(t);
					t->deCompound();
					delete t;
				}
				p->sendInventroy();
			}
		}
		return Py_True;
	}
	return Py_False;
}
api_function(setPlayerEnderChests) {
	Player* p;
	const char* x;
	if (PyArg_ParseTuple(args, "Ks:setPlayerEnderChests", &p, &x)) {
		if (PlayerCheck(p)) {
			Json::Value j = toJson(x);
			if (j.isArray()) {
				vector<ItemStack*> is = p->getEnderChestContainer()->getSlots();
				for (unsigned i = 0; i < j.size(); i++) {
					Tag* t = toTag(j[i]);
					is[i]->fromTag(t);
					t->deCompound();
					delete t;
				}
				p->sendInventroy();
				return Py_True;
			}
		}
	}
	return Py_False;
}
// 增加/移除物品
api_function(addItemEx) {
	Player* p;
	const char* x;
	if (PyArg_ParseTuple(args, "Ks:addItemEx", &p, &x)) {
		if (PlayerCheck(p)) {
			Tag* t = toTag(toJson(x));
			ItemStack i;
			i.fromTag(t);
			p->addItem(&i);
			p->sendInventroy();
			t->deCompound();
			delete t;
			return Py_True;
		}
	}
	return Py_False;
}
api_function(removeItem) {
	Player* p; int slot, num;
	if (PyArg_ParseTuple(args, "Kii:removeItem", &p, &slot, &num)) {
		if (PlayerCheck(p)) {
			p->getContainer()->clearItem(slot, num);
			p->sendInventroy();
		}
	}
	return Py_None;
}
// 设置玩家侧边栏
api_function(setSidebar) {
	Player* p;
	const char* title;
	const char* data;
	if (PyArg_ParseTuple(args, "Kss:setSidebar", &p, &title, &data)) {
		if (PlayerCheck(p)) {
			Json::Value j = toJson(data);
			setDisplayObjectivePacket(p, title);
			if (j.isObject()) {
				vector<ScorePacketInfo> info;
				for (auto& x : j.getMemberNames()) {
					ScorePacketInfo o(_scoreboard->createScoreBoardId(x),
						j[x].asInt(), x);
					info.push_back(o);
				}
				SetScorePacket(p, 0, info);
				return Py_True;
			}
		}
	}
	return Py_False;
}
api_function(removeSidebar) {
	Player* p;
	if (PyArg_ParseTuple(args, "K:removeSidebar", &p)) {
		if (PlayerCheck(p)) {
			setDisplayObjectivePacket(p, "", "");
		}
	}
	return Py_None;
}
// 根据坐标设置方块
api_function(getBlock) {
	BlockPos bp; int did;
	if (PyArg_ParseTuple(args, "iiii:getBlock", &bp.x, &bp.y, &bp.z, &did)) {
		auto bs = _level->getBlockSource(did);
		if (!bs) {
			fprintf(stderr, u8"未知纬度ID:%d", did);
			return Py_False;
		}
		auto bl = bs->getBlock(bp)->getBlockLegacy();
		return Py_BuildValue("{s:s:s:i}",
			"blockname", bl->getBlockName().c_str(),
			"blockid", (int)bl->getBlockItemID()
		);
	}
	return Py_False;
}
api_function(setBlock) {
	const char* name;
	BlockPos bp; int did;
	if (PyArg_ParseTuple(args, "siiii:setBlock", &name, &bp.x, &bp.y, &bp.z, &did)) {
		auto bs = _level->getBlockSource(did);
		if (!bs) {
			fprintf(stderr, u8"未知纬度ID:%d", did);
			return Py_False;
		}
		bs->setBlock(name, bp);
		return Py_True;
	}
	return Py_None;
}
#pragma endregion
static PyMethodDef api_list[]{
api_method(getVersion),
api_method(logout),
api_method(runcmd),
api_method(setTimer),
api_method(removeTimer),
api_method(setListener),
api_method(setShareData),
api_method(getShareData),
api_method(setCommandDescription),
api_method(getPlayerList),
api_method(sendSimpleForm),
api_method(sendModalForm),
api_method(sendCustomForm),
api_method(transferServer),
api_method(getPlayerInfo),
api_method(getActorInfo),
api_method(getActorInfoEx),
api_method(getPlayerPerm),
api_method(setPlayerPerm),
api_method(addLevel),
api_method(setName),
api_method(getPlayerScore),
api_method(modifyPlayerScore),
api_method(talkAs),
api_method(runcmdAs),
api_method(teleport),
api_method(tellraw),
api_method(tellrawEx),
api_method(setBossBar),
api_method(removeBossBar),
api_method(getScoreBoardId),
api_method(createScoreBoardId),
api_method(setDamage),
api_method(setServerMotd),
api_method(getPlayerItems),
api_method(getPlayerHand),
api_method(getPlayerItem),
api_method(setPlayerHand),
api_method(setPlayerItems),
api_method(setPlayerEnderChests),
api_method(addItemEx),
api_method(removeItem),
api_method(setSidebar),
api_method(removeSidebar),
api_method(getBlock),
api_method(setBlock),
{}
};
static PyModuleDef api_module{ PyModuleDef_HEAD_INIT, "mc", 0, -1, api_list, 0, 0, 0, 0 };
static void init() {
	//PyPreConfig cfg;
	//PyPreConfig_InitPythonConfig(&cfg);
	//cfg.utf8_mode = 1;
	//cfg.configure_locale = 0;
	//Py_PreInitialize(&cfg);
	PyImport_AppendInittab("mc", [] { return PyModule_Create(&api_module); }); //增加一个模块
	Py_Initialize();
	PyEval_InitThreads();
	if (!filesystem::exists("py"))
		filesystem::create_directory("py");
	filesystem::directory_iterator files("py");
	for (auto& info : files) {
		auto& path = info.path();
		if (path.extension() == ".py") {
			const string& name = path.stem().u8string();
			cout << "[BDSpyrunner] loading " << name << endl;
			PyImport_ImportModule(("py." + name).c_str());
			PyErr_Print();
		}
	}
	PyEval_SaveThread();
}
int DllMain(VA, int dwReason, VA) {
	if (dwReason == 1) {
		//while (1) {
		//	Tag* t = toTag(toJson(R"()"));
		//	cout << toJson(t) << endl;
		//	t->deCompound();
		//	delete t;
		//}
		init();
		puts(u8"[BDSpyrunner] 1.0.3 loaded.");
		puts(u8"[BDSpyrunner] 感谢小枫云 http://ipyvps.com 的赞助.");
	}
	return 1;
}