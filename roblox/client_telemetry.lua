--[[
    nah2 - Emetteur Client Autonome (LocalScript pour Executeur Roblox)
    Executez ce script directement depuis votre executeur (ou dossier autoexec).

    Optimisations anti-lag & latence minimale :
    - Aucun scan Workspace:GetChildren() par frame (tracking evenementiel + cache faible).
    - Cache OBB (GetBoundingBox) avec offset local sur HumanoidRootPart + pcall sans closure.
    - Pool de tables pre-allouees (candidats, entites, joints) pour supprimer la pression GC Lua.
    - Travailleurs WebSocket/HTTP statiques sans allocation de closures par frame.
]]

if not game:IsLoaded() then
	game.Loaded:Wait()
end

local CollectionService = game:GetService("CollectionService")
local HttpService = game:GetService("HttpService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")
local Workspace = game:GetService("Workspace")

-- Nettoyage d'une instance precedente si le script est re-execute
local scriptEnv = (type(getgenv) == "function" and getgenv()) or _G
if type(scriptEnv.__nah2_cleanup) == "function" then
	pcall(scriptEnv.__nah2_cleanup)
end

local CONFIG = {
	HOST = "127.0.0.1",
	PORT = 27015,
	USE_WEBSOCKET_FIRST = true,
	WS_RETRY_INTERVAL = 2.0,
	HTTP_MAX_HZ = 30, -- Plafond securite si repli HTTP (evite de saturer l'executeur)
	HTTP_FAIL_BACKOFF = 0.5,

	TRACK_TAG = "nah2_track",
	TRACK_CHARACTERS = true,
	TRACK_NESTED_NPCS = true,
	INCLUDE_LOCAL_CHARACTER = false,
	TEAM_CHECK = true,
	VISIBILITY_CHECK = true,
	MAX_VIS_CHECKS = 12,
	MAX_DISTANCE_STUDS = 1500,
	SKELETON_MAX_DISTANCE = 350, -- 0 = squelettes sur toute la portee MAX_DISTANCE_STUDS
	MAX_ENTITIES = 32,
	TARGET_HZ = 120, -- Plafond anti-lag Solara (60 FPS = 1:1 sans latence ; 0 = illimite)
	DECIMAL_SCALE = 100, -- Precision centieme de stud (JSON compact)
	OBB_CACHE_TTL = 0.75, -- Duree cache GetBoundingBox() en secondes
}

local WS_URL = string.format("ws://%s:%d/ws", CONFIG.HOST, CONFIG.PORT)
local HTTP_URL = string.format("http://%s:%d/telemetry", CONFIG.HOST, CONFIG.PORT)
local RENDER_BIND_NAME = "Nah2ExecutorTelemetryCapture"
local HTTP_HEADERS = { ["Content-Type"] = "application/json" }

local INV_DECIMAL_SCALE = 1 / CONFIG.DECIMAL_SCALE
local MAX_DIST_SQ = CONFIG.MAX_DISTANCE_STUDS * CONFIG.MAX_DISTANCE_STUDS
local SKEL_MAX_DIST_SQ = if CONFIG.SKELETON_MAX_DISTANCE > 0
	then (CONFIG.SKELETON_MAX_DISTANCE * CONFIG.SKELETON_MAX_DISTANCE)
	else math.huge

local round = math.round
local clock = os.clock
local sub = string.sub
local abs = math.abs
local jsonEncode = HttpService.JSONEncode

local R15_BONE_PARTS = {
	{ slot = 0, name = "Head" },
	{ slot = 1, name = "UpperTorso" },
	{ slot = 2, name = "LowerTorso" },
	{ slot = 3, name = "LeftUpperArm" },
	{ slot = 4, name = "LeftLowerArm" },
	{ slot = 5, name = "LeftHand" },
	{ slot = 6, name = "RightUpperArm" },
	{ slot = 7, name = "RightLowerArm" },
	{ slot = 8, name = "RightHand" },
	{ slot = 9, name = "LeftUpperLeg" },
	{ slot = 10, name = "LeftLowerLeg" },
	{ slot = 11, name = "LeftFoot" },
	{ slot = 12, name = "RightUpperLeg" },
	{ slot = 13, name = "RightLowerLeg" },
	{ slot = 14, name = "RightFoot" },
}

local R6_BONE_PARTS = {
	{ slot = 0, name = "Head" },
	{ slot = 1, name = "Torso" },
	{ slot = 2, name = "Left Arm" },
	{ slot = 3, name = "Right Arm" },
	{ slot = 4, name = "Left Leg" },
	{ slot = 5, name = "Right Leg" },
}

local function pickCallable(packed)
	for i = 1, packed.n do
		local fn = packed[i]
		if type(fn) == "function" then
			return fn
		end
	end
	return nil
end

local function safeGet(obj, key)
	if obj == nil then
		return nil
	end
	local ok, val = pcall(function()
		return obj[key]
	end)
	return if ok then val else nil
end

local function resolveWebSocketConnect()
	return pickCallable(table.pack(
		safeGet(WebSocket, "connect") or safeGet(WebSocket, "Connect"),
		safeGet(scriptEnv.WebSocket, "connect") or safeGet(scriptEnv.WebSocket, "Connect"),
		safeGet(websocket, "connect") or safeGet(websocket, "Connect"),
		safeGet(scriptEnv.websocket, "connect") or safeGet(scriptEnv.websocket, "Connect"),
		safeGet(safeGet(syn, "websocket"), "connect"),
		safeGet(safeGet(scriptEnv.syn, "websocket"), "connect"),
		safeGet(safeGet(krnl, "WebSocket"), "connect"),
		safeGet(safeGet(fluxus, "WebSocket"), "connect")
	))
end

local function resolveHttpRequest()
	return pickCallable(table.pack(
		request,
		scriptEnv.request,
		http_request,
		scriptEnv.http_request,
		httpRequest,
		scriptEnv.httpRequest,
		httprequest,
		scriptEnv.httprequest,
		safeGet(http, "request"),
		safeGet(scriptEnv.http, "request"),
		safeGet(syn, "request"),
		safeGet(scriptEnv.syn, "request"),
		safeGet(fluxus, "request"),
		safeGet(scriptEnv.fluxus, "request")
	))
end

local wsConnectFn = resolveWebSocketConnect()
local httpRequestFn = resolveHttpRequest()

if not wsConnectFn and not httpRequestFn then
	warn("[nah2] Aucun transport executeur compatible detecte (WebSocket.connect / request introuvable).")
	return
end

local running = true
local connections = {}
local boundRenderStep = false

local stableIds = setmetatable({}, { __mode = "k" })
local cachedNpcModels = setmetatable({}, { __mode = "k" })
local rootPartCache = setmetatable({}, { __mode = "k" })
local jointPartsCache = setmetatable({}, { __mode = "k" })
local obbCache = setmetatable({}, { __mode = "k" })
local taggedCache = setmetatable({}, { __mode = "k" })

local nextEntityId = 1
local frameSeq = 0
local lastCaptureClock = 0

-- Buffers pre-alloues pour limiter la pression GC sur le thread de rendu
local camP = { 0, 0, 0 }
local camR = { 1, 0, 0 }
local camU = { 0, 1, 0 }
local camL = { 0, 0, -1 }
local camVp = { 1280, 720 }
local camMl = { 640, 360 }
local camBuffer = {
	p = camP,
	r = camR,
	u = camU,
	l = camL,
	fov = 70,
	vp = camVp,
	mb = 0,
	ui = 0,
	ml = camMl,
	sens = nil,
}
local entsBuffer = {}
local prevEntCount = 0
local candidatePool = {}
local entryPool = {}
local jointArraysPool = {}
local velArraysPool = {}

local function acquireEntrySlot(slotIdx)
	local entry = entryPool[slotIdx]
	if not entry then
		entry = {
			id = 0,
			n = "",
			k = 0,
			p = { 0, 0, 0 },
			s = { 0, 0, 0 },
			r = { 1, 0, 0 },
			u = { 0, 1, 0 },
			l = { 0, 0, -1 },
			rig = nil,
			j = nil,
			hp = nil,
			mhp = nil,
			ally = nil,
			vis = nil,
			v = nil,
		}
		entryPool[slotIdx] = entry
		jointArraysPool[slotIdx] = table.create(60)
		velArraysPool[slotIdx] = { 0, 0, 0 }
	end
	return entry
end

local payloadBuffer = {
	seq = 0,
	ts = 0,
	cam = camBuffer,
	ents = entsBuffer,
}

local hasDirtyFrame = false
local inFlightSend = false
local lastHttpSentAt = 0
local httpBackoffUntil = 0

local wsClient = nil
local wsOpen = false
local wsConnecting = false
local wsCapable = CONFIG.USE_WEBSOCKET_FIRST and (wsConnectFn ~= nil)
local lastWsAttempt = -100

local function q(value)
	if value ~= value or abs(value) == math.huge then
		return 0
	end
	return round(value * CONFIG.DECIMAL_SCALE) * INV_DECIMAL_SCALE
end

local function writeVec3(dst, v)
	dst[1] = q(v.X)
	dst[2] = q(v.Y)
	dst[3] = q(v.Z)
end

local function getStableId(inst)
	local existing = stableIds[inst]
	if existing then
		return existing
	end
	local id = nextEntityId
	nextEntityId += 1
	stableIds[inst] = id
	return id
end

local function registerHumanoidDescendant(desc)
	if desc:IsA("Humanoid") then
		local parent = desc.Parent
		if parent and parent:IsA("Model") then
			if CONFIG.TRACK_NESTED_NPCS or parent.Parent == Workspace then
				cachedNpcModels[parent] = true
			end
		end
	end
end

local function onWorkspaceDescendantRemoving(desc)
	if cachedNpcModels[desc] then
		cachedNpcModels[desc] = nil
	end
	obbCache[desc] = nil
	rootPartCache[desc] = nil
	jointPartsCache[desc] = nil
end

for _, taggedInst in ipairs(CollectionService:GetTagged(CONFIG.TRACK_TAG)) do
	taggedCache[taggedInst] = true
end
table.insert(connections, CollectionService:GetInstanceAddedSignal(CONFIG.TRACK_TAG):Connect(function(inst)
	taggedCache[inst] = true
end))
table.insert(connections, CollectionService:GetInstanceRemovedSignal(CONFIG.TRACK_TAG):Connect(function(inst)
	taggedCache[inst] = nil
end))

if CONFIG.TRACK_CHARACTERS then
	table.insert(connections, Workspace.DescendantAdded:Connect(registerHumanoidDescendant))
	table.insert(connections, Workspace.DescendantRemoving:Connect(onWorkspaceDescendantRemoving))
	task.spawn(function()
		if CONFIG.TRACK_NESTED_NPCS then
			local descendants = Workspace:GetDescendants()
			for i = 1, #descendants do
				if not running then
					break
				end
				registerHumanoidDescendant(descendants[i])
				if i % 800 == 0 then
					task.wait()
				end
			end
		else
			local children = Workspace:GetChildren()
			for i = 1, #children do
				if not running then
					break
				end
				local child = children[i]
				if child:IsA("Model") and child:FindFirstChildOfClass("Humanoid") then
					cachedNpcModels[child] = true
				end
				if i % 400 == 0 then
					task.wait()
				end
			end
		end
	end)
end

local function closeWebSocket(sock)
	local target = sock or wsClient
	if not target then
		return
	end
	if target == wsClient then
		wsClient = nil
		wsOpen = false
	end
	pcall(function()
		target:Close()
	end)
end

local function tryInitWebSocket()
	if not running or not wsCapable or not wsConnectFn or wsClient or wsConnecting then
		return
	end
	local now = clock()
	if (now - lastWsAttempt) < CONFIG.WS_RETRY_INTERVAL then
		return
	end
	lastWsAttempt = now
	wsConnecting = true

	task.spawn(function()
		local ok, result = pcall(wsConnectFn, WS_URL)
		wsConnecting = false

		if not running then
			if ok and result then
				pcall(function()
					result:Close()
				end)
			end
			return
		end

		if not ok or not result then
			local errMsg = string.lower(tostring(result))
			if string.find(errMsg, "not implemented") or string.find(errMsg, "unsupported") then
				wsCapable = false
				warn("[nah2] WebSocket non supporte par cet executeur -> mode HTTP POST actif.")
			end
			return
		end

		wsClient = result
		wsOpen = true

		pcall(function()
			local onClose = result.OnClose
			if onClose then
				local connectFn = onClose.Connect or onClose.connect
				if type(connectFn) == "function" then
					connectFn(onClose, function()
						if wsClient == result then
							wsClient = nil
							wsOpen = false
						end
					end)
				end
			end
		end)
	end)
end

local httpRequestPayload = {
	Url = HTTP_URL,
	Method = "POST",
	Headers = HTTP_HEADERS,
	Body = "",
}

local function wsSendWorker(sock, json)
	local okSend, sendResult = pcall(sock.Send, sock, json)
	if not okSend or sendResult == false then
		closeWebSocket(sock)
	end
	inFlightSend = false
end

local function httpSendWorker()
	local okReq, res = pcall(httpRequestFn, httpRequestPayload)
	local succeeded = okReq and type(res) == "table" and (res.Success == true or res.StatusCode == 204 or res.StatusCode == 200)
	if not succeeded then
		httpBackoffUntil = clock() + CONFIG.HTTP_FAIL_BACKOFF
	end
	inFlightSend = false
end

local function flushLatestFrame()
	if not running or inFlightSend or not hasDirtyFrame then
		return
	end

	-- 1. Envoi WebSocket prioritaire (latence sub-milliseconde, zero closure par frame)
	if wsCapable then
		if not wsClient and not wsConnecting then
			tryInitWebSocket()
		end
		if wsClient and wsOpen then
			local sock = wsClient
			hasDirtyFrame = false
			local okEncode, json = pcall(jsonEncode, HttpService, payloadBuffer)
			if not okEncode or type(json) ~= "string" then
				return
			end

			inFlightSend = true
			task.spawn(wsSendWorker, sock, json)
			return
		end
	end

	-- 2. Repli HTTP POST via l'executeur
	if not httpRequestFn then
		return
	end

	local now = clock()
	if now < httpBackoffUntil then
		return
	end
	if CONFIG.HTTP_MAX_HZ > 0 and (now - lastHttpSentAt) < (1.0 / CONFIG.HTTP_MAX_HZ) then
		return
	end

	hasDirtyFrame = false
	local okEncode, json = pcall(jsonEncode, HttpService, payloadBuffer)
	if not okEncode or type(json) ~= "string" then
		return
	end

	inFlightSend = true
	lastHttpSentAt = now
	httpRequestPayload.Body = json
	task.spawn(httpSendWorker)
end

local function getModelApproxInfo(model)
	local cached = rootPartCache[model]
	if cached then
		local hum = cached.hum
		if not hum or hum.Parent ~= model then
			hum = model:FindFirstChildOfClass("Humanoid")
			cached.hum = hum
		end
		local root = cached.root
		if root and root.Parent == model then
			return root.Position, root, hum
		end
	end

	local hum = model:FindFirstChildOfClass("Humanoid")
	local hrp = model:FindFirstChild("HumanoidRootPart")
	local root = if hrp and hrp:IsA("BasePart") then hrp else model.PrimaryPart
	rootPartCache[model] = {
		root = root,
		hum = hum,
	}

	if root then
		return root.Position, root, hum
	end
	local okPivot, pivot = pcall(model.GetPivot, model)
	if okPivot and pivot then
		return pivot.Position, nil, hum
	end
	return nil, nil, hum
end

local function resolveModelJointCache(model, rigType, now)
	local mapping = if rigType == Enum.HumanoidRigType.R15 then R15_BONE_PARTS else R6_BONE_PARTS
	local rigCode = if rigType == Enum.HumanoidRigType.R15 then 15 else 6
	local parts = {}
	local count = 0
	for i = 1, #mapping do
		local entry = mapping[i]
		local part = model:FindFirstChild(entry.name)
		if part and part:IsA("BasePart") then
			count += 1
			parts[count] = { slot = entry.slot, part = part }
		end
	end
	local rec = {
		rigType = rigType,
		rigCode = rigCode,
		expected = #mapping,
		parts = parts,
		nextRefresh = now + 0.5,
	}
	jointPartsCache[model] = rec
	return rec
end

local function extractJointsCached(model, rigType, now, dstJoints)
	local rec = jointPartsCache[model]
	if not rec or rec.rigType ~= rigType or (#rec.parts < rec.expected and now >= rec.nextRefresh) then
		rec = resolveModelJointCache(model, rigType, now)
	end

	local jIdx = 0
	local parts = rec.parts
	local invalidated = false

	for i = 1, #parts do
		local item = parts[i]
		local part = item.part
		if part.Parent == model then
			local pos = part.Position
			dstJoints[jIdx + 1] = item.slot
			dstJoints[jIdx + 2] = q(pos.X)
			dstJoints[jIdx + 3] = q(pos.Y)
			dstJoints[jIdx + 4] = q(pos.Z)
			jIdx += 4
		else
			invalidated = true
		end
	end

	for k = jIdx + 1, #dstJoints do
		dstJoints[k] = nil
	end

	if invalidated then
		jointPartsCache[model] = nil
	end

	return rec.rigCode
end

local function getModelBoxCached(model, primary, now)
	local cached = obbCache[model]
	if cached and primary and cached.primary == primary and now < cached.expiresAt then
		return primary.CFrame * cached.localOffset, cached.size
	end

	local ok, cf, size = pcall(model.GetBoundingBox, model)
	if ok and cf and size then
		if primary then
			local pCf = primary.CFrame
			if cached then
				cached.primary = primary
				cached.localOffset = pCf:ToObjectSpace(cf)
				cached.size = size
				cached.expiresAt = now + CONFIG.OBB_CACHE_TTL
			else
				obbCache[model] = {
					primary = primary,
					localOffset = pCf:ToObjectSpace(cf),
					size = size,
					expiresAt = now + CONFIG.OBB_CACHE_TTL,
				}
			end
		else
			obbCache[model] = nil
		end
		return cf, size
	end

	if primary then
		return primary.CFrame, primary.Size
	end
	return nil, nil
end

local visRayParams = RaycastParams.new()
visRayParams.FilterType = Enum.RaycastFilterType.Exclude
visRayParams.IgnoreWater = true
local visFilterList = {}
local lastVisLocalChar = nil
local lastVisCamera = nil

local cachedMouseSens = 0
local nextSensRefresh = 0

local function readMouseSens(now)
	if now < nextSensRefresh then
		return cachedMouseSens
	end
	nextSensRefresh = now + 1.0
	local ok, val = pcall(function()
		return UserSettings():GetService("UserGameSettings").MouseSensitivity
	end)
	if ok and type(val) == "number" and val > 0 then
		cachedMouseSens = q(val)
	end
	return cachedMouseSens
end

local function isPlayerAlly(localPlr, otherPlr)
	if not CONFIG.TEAM_CHECK or not localPlr or not otherPlr or localPlr == otherPlr then
		return false
	end
	if localPlr.Neutral == false and otherPlr.Neutral == false then
		if localPlr.Team ~= nil and otherPlr.Team ~= nil then
			return localPlr.Team == otherPlr.Team
		end
		if localPlr.TeamColor ~= nil and otherPlr.TeamColor ~= nil then
			return localPlr.TeamColor == otherPlr.TeamColor
		end
	end
	return false
end

local function isPointVisible(origin, targetPos, targetModel)
	local curOrigin = origin
	for _ = 1, 2 do
		local remaining = targetPos - curOrigin
		if remaining.Magnitude < 0.25 then
			return true
		end
		local res = Workspace:Raycast(curOrigin, remaining, visRayParams)
		if not res then
			return true
		end
		local hit = res.Instance
		if hit:IsDescendantOf(targetModel) then
			return true
		end
		if not hit:IsA("Terrain") and (hit.Transparency >= 0.85 or (not hit.CanCollide and hit.Transparency >= 0.45)) then
			curOrigin = res.Position + remaining.Unit * 0.05
		else
			return false
		end
	end
	return false
end

local charToPlayerScratch = {}

local function fillEntityEntry(outSlotIdx, candidate, camPos, camLook, localPlayer, visBudgetRef, now)
	local inst = candidate.inst
	if candidate.isModel then
		local humanoid = candidate.hum
		local primary = candidate.root

		local cf, size = getModelBoxCached(inst, primary, now)
		if not cf or not size then
			return nil
		end

		local center = cf.Position
		local dx = center.X - camPos.X
		local dy = center.Y - camPos.Y
		local dz = center.Z - camPos.Z
		local distSq = dx * dx + dy * dy + dz * dz
		if distSq > MAX_DIST_SQ then
			return nil
		end

		local entry = acquireEntrySlot(outSlotIdx)
		entry.id = getStableId(inst)
		entry.n = sub(inst.Name, 1, 32)
		entry.k = if humanoid then 1 else 0
		writeVec3(entry.p, center)
		writeVec3(entry.s, size)
		writeVec3(entry.r, cf.RightVector)
		writeVec3(entry.u, cf.UpVector)
		writeVec3(entry.l, cf.LookVector)

		if humanoid then
			entry.hp = q(humanoid.Health)
			entry.mhp = q(humanoid.MaxHealth)
		else
			entry.hp = nil
			entry.mhp = nil
		end

		local otherPlr = charToPlayerScratch[inst]
		entry.ally = if isPlayerAlly(localPlayer, otherPlr) then 1 else nil

		if primary then
			local vBuf = velArraysPool[outSlotIdx]
			writeVec3(vBuf, primary.AssemblyLinearVelocity)
			entry.v = vBuf
		else
			entry.v = nil
		end

		local headPos = nil
		if humanoid and humanoid.Health > 0 and distSq <= SKEL_MAX_DIST_SQ then
			local jBuf = jointArraysPool[outSlotIdx]
			entry.rig = extractJointsCached(inst, humanoid.RigType, now, jBuf)
			entry.j = jBuf
			if jBuf[1] == 0 and jBuf[2] then
				headPos = Vector3.new(jBuf[2], jBuf[3], jBuf[4])
			end
		else
			entry.rig = nil
			entry.j = nil
		end

		if CONFIG.VISIBILITY_CHECK and humanoid and humanoid.Health > 0 and visBudgetRef.rem > 0 then
			local forward = dx * camLook.X + dy * camLook.Y + dz * camLook.Z
			if forward > 0 then
				visBudgetRef.rem -= 1
				local testHead = headPos or (center + cf.UpVector * (size.Y * 0.36))
				if isPointVisible(camPos, testHead, inst) or isPointVisible(camPos, center, inst) then
					entry.vis = 1
				else
					entry.vis = 0
				end
			else
				entry.vis = nil
			end
		else
			entry.vis = nil
		end

		return entry
	else
		local pos = inst.Position
		local dx = pos.X - camPos.X
		local dy = pos.Y - camPos.Y
		local dz = pos.Z - camPos.Z
		if (dx * dx + dy * dy + dz * dz) > MAX_DIST_SQ then
			return nil
		end
		local cf = inst.CFrame
		local entry = acquireEntrySlot(outSlotIdx)
		entry.id = getStableId(inst)
		entry.n = sub(inst.Name, 1, 32)
		entry.k = 0
		writeVec3(entry.p, pos)
		writeVec3(entry.s, inst.Size)
		writeVec3(entry.r, cf.RightVector)
		writeVec3(entry.u, cf.UpVector)
		writeVec3(entry.l, cf.LookVector)
		entry.rig = nil
		entry.j = nil
		entry.hp = nil
		entry.mhp = nil
		entry.ally = nil
		entry.vis = nil
		entry.v = nil
		return entry
	end
end

local seenScratch = {}
local candidatesScratch = {}
local visBudgetState = { rem = 0 }

local function compareCandidates(a, b)
	return a.distSq < b.distSq
end

local function collectEntities(camPos, camLook, camera, now)
	local localPlayer = Players.LocalPlayer
	local localChar = if localPlayer then localPlayer.Character else nil
	if localChar ~= lastVisLocalChar or camera ~= lastVisCamera then
		lastVisLocalChar = localChar
		lastVisCamera = camera
		table.clear(visFilterList)
		if localChar then
			table.insert(visFilterList, localChar)
		end
		if camera then
			table.insert(visFilterList, camera)
		end
		visRayParams.FilterDescendantsInstances = visFilterList
	end

	table.clear(seenScratch)
	table.clear(candidatesScratch)
	table.clear(charToPlayerScratch)
	local candCount = 0

	local function tryQueue(inst)
		if seenScratch[inst] then
			return true
		end
		seenScratch[inst] = true
		if not inst.Parent or not inst:IsDescendantOf(Workspace) then
			return false
		end
		if not CONFIG.INCLUDE_LOCAL_CHARACTER and localChar == inst then
			return true
		end

		if inst:IsA("Model") then
			local approxPos, root, hum = getModelApproxInfo(inst)
			if not approxPos then
				return true
			end
			local dx = approxPos.X - camPos.X
			local dy = approxPos.Y - camPos.Y
			local dz = approxPos.Z - camPos.Z
			local distSq = dx * dx + dy * dy + dz * dz
			if distSq <= MAX_DIST_SQ then
				candCount += 1
				local slot = candidatePool[candCount]
				if not slot then
					slot = {}
					candidatePool[candCount] = slot
				end
				slot.distSq = distSq
				slot.inst = inst
				slot.isModel = true
				slot.root = root
				slot.hum = hum
				candidatesScratch[candCount] = slot
			end
		elseif inst:IsA("BasePart") then
			local pos = inst.Position
			local dx = pos.X - camPos.X
			local dy = pos.Y - camPos.Y
			local dz = pos.Z - camPos.Z
			local distSq = dx * dx + dy * dy + dz * dz
			if distSq <= MAX_DIST_SQ then
				candCount += 1
				local slot = candidatePool[candCount]
				if not slot then
					slot = {}
					candidatePool[candCount] = slot
				end
				slot.distSq = distSq
				slot.inst = inst
				slot.isModel = false
				slot.root = nil
				slot.hum = nil
				candidatesScratch[candCount] = slot
			end
		end
		return true
	end

	-- 1. Instances explicitement taguees ("nah2_track") via cache evenementiel
	for inst in pairs(taggedCache) do
		if not tryQueue(inst) then
			taggedCache[inst] = nil
		end
	end

	-- 2. Personnages joueurs et NPCs via cache evenementiel (sans Workspace:GetChildren() par frame)
	if CONFIG.TRACK_CHARACTERS then
		local playerList = Players:GetPlayers()
		for i = 1, #playerList do
			local plr = playerList[i]
			local char = plr.Character
			if char then
				charToPlayerScratch[char] = plr
				tryQueue(char)
			end
		end

		for model in pairs(cachedNpcModels) do
			if not tryQueue(model) then
				cachedNpcModels[model] = nil
			end
		end
	end

	if candCount > CONFIG.MAX_VIS_CHECKS or candCount > CONFIG.MAX_ENTITIES then
		table.sort(candidatesScratch, compareCandidates)
	end

	visBudgetState.rem = CONFIG.MAX_VIS_CHECKS
	local outIdx = 0
	local limit = math.min(candCount, CONFIG.MAX_ENTITIES)
	for i = 1, limit do
		local entry = fillEntityEntry(outIdx + 1, candidatesScratch[i], camPos, camLook, localPlayer, visBudgetState, now)
		if entry then
			outIdx += 1
			entsBuffer[outIdx] = entry
		end
	end
	for i = outIdx + 1, prevEntCount do
		entsBuffer[i] = nil
	end
	prevEntCount = outIdx
	return entsBuffer
end

local function onRenderStep()
	if not running then
		return
	end

	local now = clock()
	if CONFIG.TARGET_HZ > 0 and (now - lastCaptureClock) < math.max(0, (1 / CONFIG.TARGET_HZ) - 0.001) then
		return
	end

	local camera = Workspace.CurrentCamera
	if not camera then
		return
	end

	lastCaptureClock = now
	frameSeq += 1

	local camCf = camera.CFrame
	local camPos = camCf.Position
	local camLook = camCf.LookVector
	local vp = camera.ViewportSize

	writeVec3(camP, camPos)
	writeVec3(camR, camCf.RightVector)
	writeVec3(camU, camCf.UpVector)
	writeVec3(camL, camLook)
	camBuffer.fov = q(camera.FieldOfView)
	camVp[1] = round(vp.X)
	camVp[2] = round(vp.Y)

	local mbEnum = UserInputService.MouseBehavior
	if mbEnum == Enum.MouseBehavior.LockCenter then
		camBuffer.mb = 1
	elseif mbEnum == Enum.MouseBehavior.LockCurrentPosition then
		camBuffer.mb = 2
	else
		camBuffer.mb = 0
	end
	camBuffer.ui = if UserInputService:GetFocusedTextBox() ~= nil then 1 else 0

	local okMl, ml = pcall(UserInputService.GetMouseLocation, UserInputService)
	if okMl and ml then
		camMl[1] = q(ml.X)
		camMl[2] = q(ml.Y)
		camBuffer.ml = camMl
	else
		camBuffer.ml = nil
	end

	local sens = readMouseSens(now)
	camBuffer.sens = if sens > 0 then sens else nil

	payloadBuffer.seq = frameSeq
	payloadBuffer.ts = q(now)
	payloadBuffer.ents = collectEntities(camPos, camLook, camera, now)
	hasDirtyFrame = true

	flushLatestFrame()
end

local function cleanup()
	if not running then
		return
	end
	running = false
	if boundRenderStep then
		pcall(RunService.UnbindFromRenderStep, RunService, RENDER_BIND_NAME)
		boundRenderStep = false
	end
	for _, conn in ipairs(connections) do
		pcall(function()
			conn:Disconnect()
		end)
	end
	table.clear(connections)
	closeWebSocket()
	if scriptEnv.__nah2_cleanup == cleanup then
		scriptEnv.__nah2_cleanup = nil
	end
end

scriptEnv.__nah2_cleanup = cleanup

pcall(RunService.UnbindFromRenderStep, RunService, RENDER_BIND_NAME)
local bindOk = pcall(RunService.BindToRenderStep, RunService, RENDER_BIND_NAME, Enum.RenderPriority.Last.Value + 1, onRenderStep)
if bindOk then
	boundRenderStep = true
else
	table.insert(connections, RunService.RenderStepped:Connect(onRenderStep))
end

table.insert(connections, RunService.Heartbeat:Connect(flushLatestFrame))

if CONFIG.USE_WEBSOCKET_FIRST and wsConnectFn then
	tryInitWebSocket()
end

print(string.format(
	"[nah2] Emetteur executeur (mode latence minimale) -> %s:%d (WS=%s, HTTP=%s)",
	CONFIG.HOST,
	CONFIG.PORT,
	tostring(wsConnectFn ~= nil),
	tostring(httpRequestFn ~= nil)
))
