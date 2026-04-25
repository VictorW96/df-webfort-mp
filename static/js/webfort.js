/*
 * webfort.js
 * Copyright (c) 2014 mifki, ISC license.
 */

/*jslint browser:true */

var params = getParams();
var colors = [
	32, 39, 49,
	0, 106, 255,
	68, 184, 57,
	114, 156, 251,
	212, 54, 85,
	176, 50, 255,
	217, 118, 65,
	170, 196, 178,
	128, 151, 156,
	48, 165, 255,
	144, 255, 79,
	168, 212, 255,
	255, 82, 82,
	255, 107, 255,
	255, 232, 102,
	255, 250, 232
];

var MAX_FPS = 20;

// Sprite atlas for graphics mode (USE_GRAPHICS=true).
// Not used in ASCII/curses mode — atlas is never fetched when graphicsMode is false.
// atlasImg  – HTMLImageElement (or null if not loaded yet)
// atlasMap  – object: texpos_string → [atlas_x, atlas_y]
// atlasTW   – sprite pixel width in the atlas
// atlasTH   – sprite pixel height in the atlas
var atlasImg = null;
var atlasMap = null;
var atlasTW  = 16;
var atlasTH  = 16;
// Last atlas version counter received from the server (bits[4..7] of tock byte[2]).
// When it changes we re-fetch atlas.png/atlas.json (graphics mode only).
var lastAtlasVersion = -1;
var atlasLoading = false;

var host = params.host;
var port = params.port;
var tileSet = params.tiles;
var textSet = params.text;
var colorscheme = params.colors;
var nick = params.nick;
var secret = params.secret;

var wsUri = 'ws://' + host + ':' + port +
	'/' + encodeURIComponent(nick) +
	'/' + encodeURIComponent(secret);
var active = false;
var websocket = null;
// True when the server reports a non-dwarfmode screen is active (menus,
// dialogs, setup screens). Used to route WASD and scroll to camera vs. game.
var isInMenu = false;
// Camera position reported by the server for this client's viewport.
var camX = 0, camY = 0, camZ = 0;
// Map size in tiles (received via opcode 118 on connect).
var mapW = 0, mapH = 0, mapD = 0;
var lastFrame = 0;

var tilew  = 16;
var tileh  = 16;
var dimx   = 80;
var dimy   = 25;

var cmd = {
	"update":       110,
	"sendKey":      111,
	"cursorsUpdate": 112,
	"sendMouse":    113,
	"cursorMove":   114,
	"connect":      115,
	"requestTurn":  116,
	"camMove":      117,
	"mapInfo":      118
};

// Converts integer value in seconds to a time string, HH:MM:SS
function toTime(n) {
	var h = Math.floor(n / 60  / 60);
	var m = Math.floor(n / 60) % 60;
	var s = n % 60;
	return ("0" + h).slice(-2) + ":" +
	       ("0" + m).slice(-2) + ":" +
	       ("0" + s).slice(-2);
}

function plural(n, unit)
{
	return n + " " + unit + (n === 1 ? "" : "s");
}

// Converts an integer value in ticks to the dwarven calendar
function toGameTime(n) {
	var years = Math.floor(n / 12 / 28 / 1200);
	var months = Math.floor(n / 28 / 1200) % 12;
	var days = Math.floor(n / 1200) % 28;
	var ticks = n % 1200;

	var times = [];
	if (years > 0) {
		times.push(plural(years, "year"));
	}
	if (months > 0) {
		times.push(plural(months, "month"));
	} else if (days > 0) {
		times.push(plural(days, "day"));
	} else {
		times.push(plural(ticks, "tick"));
	}

	return times.join(", ");
}

function setStats(userCount, ingame_time, timeLeft) {
	var u = document.getElementById('user-count');
	var t = document.getElementById('time-left');
	u.innerHTML = String(userCount) + " <i class='fa fa-users'></i>";

	if (timeLeft === -1) {
		t.innerHTML = "";
	} else {
		t.innerHTML = (ingame_time? toGameTime(timeLeft) : toTime(timeLeft)) +
			" <i class='fa fa-clock-o'></i>";
	}
}

function setStatus(text, color, onclick) {
	var m = document.getElementById('message');
	m.innerHTML = text;
	var st = m.parentNode;
	// Use .onclick (replaces previous handler) instead of addEventListener
	// (which accumulates a new listener every tock frame and causes multiple
	// requestTurn messages on a single click).
	st.onclick = onclick || null;
	st.style.backgroundColor = color;
}

// Fetch the sprite atlas from the server.
// Only called in graphics mode (USE_GRAPHICS=true). Never called in ASCII mode.
// Triggered by atlas version counter changes in tock() (world load, mode switch).
// Retries every 3 seconds until the atlas is ready (builds after ~60 ticks).
function loadAtlas() {
	if (atlasLoading) return;
	atlasLoading = true;
	var base = 'http://' + host + ':' + port;
	var ts = Date.now();
	var jsonReq = new XMLHttpRequest();
	jsonReq.open('GET', base + '/atlas.json?t=' + ts);
	jsonReq.onload = function() {
		if (jsonReq.status === 404) {
			// Atlas not built yet — retry after 3 s.
			atlasLoading = false;
			setTimeout(loadAtlas, 3000);
			return;
		}
		if (jsonReq.status !== 200) { atlasLoading = false; return; }
		try {
			var info = JSON.parse(jsonReq.responseText);
			atlasTW = info.tw || 16;
			atlasTH = info.th || 16;
			// Load the PNG; only swap atlasImg+atlasMap atomically once the
			// image is fully decoded so we never draw with a stale map.
			var newMap = info.map || null;
			var img = new Image();
			img.onload = function() {
				atlasImg = img;
				atlasMap = newMap;
				atlasLoading = false;
			};
			img.onerror = function() { atlasLoading = false; };
			img.src = base + '/atlas.png?t=' + ts;
		} catch(e) {
			console.warn('webfort: atlas.json parse error', e);
			atlasLoading = false;
		}
	};
	jsonReq.onerror = function() { atlasLoading = false; setTimeout(loadAtlas, 3000); };
	jsonReq.send();
}

function connect() {
	setStatus('Connecting...', 'orange');
	websocket = new WebSocket(wsUri, ['WebFortress-v2.0', 'WebFortress-invalid']);
	websocket.binaryType = 'arraybuffer';
	websocket.onopen  = onOpen;
	websocket.onclose = onClose;
	websocket.onerror = onError;
}

function onOpen(evt) {
	setStatus('Connected, initializing...', 'orange');

	websocket.send(new Uint8Array([cmd.connect]));
	websocket.send(new Uint8Array([cmd.update]));
	websocket.onmessage = onMessage;
	// Atlas is fetched lazily on the first tock that reports graphicsMode=true.
}

var isError = false;
function onClose(evt) {
	if (isError) {
		isError = false;
		setStatus('Connection Error. Click to retry', 'red', connect);
	} else if (evt.reason) {
		setStatus(evt.reason + ' Click to try again.', 'red', connect);
	} else {
		setStatus('Unknown disconnect: Check the console (Ctrl-Shift-J), then click to reconnect.', 'red', connect);
	}
}

function onError(ev) {
	isError = true;
}

function requestTurn() {
	websocket.send(new Uint8Array([cmd.requestTurn]));
}

// Send a camera pan delta to the server (opcode 117).
// dx/dy/dz are signed integers in [-128, 127]; packed as uint8 two's complement.
function sendCamMove(dx, dy, dz) {
	if (!websocket || websocket.readyState !== WebSocket.OPEN) return;
	websocket.send(new Uint8Array([
		cmd.camMove,
		dx & 0xFF,
		dy & 0xFF,
		dz & 0xFF
	]));
}

function renderQueueStatus(s) {
	if (s.isActive) {
		active = true;
		setStatus("You're in charge now! Click here to end your turn.", 'green', requestTurn);
	} else if (s.isNoPlayer) {
		active = false;
		setStatus("Nobody is playing right now. Click here to ask for a turn.", 'grey', requestTurn);
	} else {
		active = false;
		var displayedName = s.currentPlayer || "Somebody else";
		setStatus(displayedName +" is doing their best. Please wait warmly.", 'orange');
	}
	setStats(s.playerCount, s.ingameTime, s.timeLeft);
}

function renderUpdate(ctx, data, offset, graphicsMode) {
	var t = [];
	var k;
	var x;
	var y;
	var s;
	var bg;
	var fg;
	// zoneW/zoneH: pixel size of one color zone in the 1024×1024 colorized
	// tileset canvas (16 tiles × tilew pixels = 256 for standard 16px tiles).
	var zoneW = tilew * tilew;
	var zoneH = tileh * tileh;

	for (k = offset; k < data.length; k += 9) {
		x = data[k + 0];
		y = data[k + 1];

		s  = data[k + 2]; // char
		bg = data[k + 3] % tilew;
		fg = data[k + 4];

		var texpos       = data[k + 5] | (data[k + 6] << 8);
		var texpos_lower = data[k + 7] | (data[k + 8] << 8);

		// Sprite path: graphical mode with a real sprite texpos.
		// In ASCII/curses mode graphicsMode is false and we always use CP437.
		// In graphical mode we use the atlas for any tile with texpos > 0;
		// tiles with texpos == 0 (pure text/UI) fall through to the CP437 path.
		// Falls back to CP437 if the texpos is not in the atlas (e.g. during
		// an atlas rebuild after a world load).
		if (graphicsMode && texpos > 0 && atlasImg && atlasMap) {
			var dst_x = x * tilew;
			var dst_y = y * tileh;

			var lkey = String(texpos_lower);
			var ukey = String(texpos);
			var hasLower = texpos_lower > 0 && atlasMap[lkey];
			var hasUpper = !!atlasMap[ukey];

			// Only use the sprite path if at least one sprite was found.
			// If neither is in the atlas (e.g. world-load cleared surfaces
			// and the atlas hasn't rebuilt yet), fall through to CP437 so
			// the map isn't blank during the rebuild window.
			if (hasLower || hasUpper) {
				// Draw background color for transparent sprite regions.
				var bg_x = ((bg % 4) * zoneW) + 15 * tilew;
				var bg_y = (Math.floor(bg / 4) * zoneH) + 15 * tileh;
				ctx.drawImage(cd, bg_x, bg_y, tilew, tileh, dst_x, dst_y, tilew, tileh);

				// Lower (terrain) sprite.
				if (hasLower) {
					var lpos = atlasMap[lkey];
					ctx.drawImage(atlasImg, lpos[0], lpos[1], atlasTW, atlasTH,
					              dst_x, dst_y, tilew, tileh);
				}

				// Upper (entity / item) sprite.
				if (hasUpper) {
					var upos = atlasMap[ukey];
					ctx.drawImage(atlasImg, upos[0], upos[1], atlasTW, atlasTH,
					              dst_x, dst_y, tilew, tileh);
				}
				continue;
			}
			// Fall through to CP437 rendering below.
		}

		// --- CP437 text path ---
		var bg_x = ((bg % 4) * zoneW) + 15 * tilew;
		var bg_y = (Math.floor(bg / 4) * zoneH) + 15 * tileh;
		ctx.drawImage(cd,
			bg_x, bg_y, tilew, tileh,
			x * tilew, y * tileh, tilew, tileh);

		if (data[k + 3] & 64) {
			t.push(k);
			continue;
		}
		var fg_x = (s % 16) * tilew + ((fg % 4) * zoneW);
		var fg_y = Math.floor(s / 16) * tileh + (Math.floor(fg / 4) * zoneH);
		ctx.drawImage(cd,
			fg_x, fg_y, tilew, tileh,
			x * tilew, y * tileh, tilew, tileh);
	}

	for (var m = 0; m < t.length; m++) {
		k = t[m];
		x = data[k + 0];
		y = data[k + 1];

		s  = data[k + 2];
		bg = data[k + 3];
		fg = data[k + 4];

		var i = (s % 16) * tilew + ((fg % 4) * zoneW);
		var j = Math.floor(s / 16) * tileh + (Math.floor(fg / 4) * zoneH);
		ctx.drawImage(ct,
			i, j, tilew, tileh,
			x * tilew, y * tileh, tilew, tileh);
	}
}

function onMessage(evt) {
	var data = new Uint8Array(evt.data);

	var ctx = canvas.getContext('2d');
	if (data[0] === cmd.update) {
		if (stats) { stats.begin(); }
		var gameStatus = {};
		gameStatus.playerCount = data[1] & 127;

		// [2] bits — same meaning as before
		gameStatus.isActive   = (data[2] & 1) !== 0;
		gameStatus.isNoPlayer = (data[2] & 2) !== 0;
		gameStatus.ingameTime = (data[2] & 4) !== 0;
		var graphicsMode      = (data[2] & 8) !== 0;
		// Atlas version is in bits[4..7]. Re-fetch only in graphics mode.
		// In ASCII mode the atlas is never fetched to avoid spurious 404 errors.
		var atlasVersion = (data[2] >> 4) & 0x0F;
		if (graphicsMode && atlasVersion !== lastAtlasVersion) {
			lastAtlasVersion = atlasVersion;
			loadAtlas();
		} else if (!graphicsMode) {
			// Clear stale atlas state when switching back to ASCII mode.
			lastAtlasVersion = -1;
			atlasImg = null;
			atlasMap = null;
		}

		// [3] flags2: bit 0 = non-dwarfmode screen active (menu/dialog)
		isInMenu = (data[3] & 1) !== 0;

		// [4-7] time left, in seconds. -1 if no timer.
		gameStatus.timeLeft =
			(data[4]<<0) |
			(data[5]<<8) |
			(data[6]<<16) |
			(data[7]<<24);

		// [8-9] game dimensions
		var neww = data[8] * tilew;
		var newh = data[9] * tileh;
		var newDimx = data[8];
		var newDimy = data[9];
		if (newDimx > 0) { dimx = newDimx; dimy = newDimy; }

		// Resize canvas to match the reported DF screen dimensions.
		if (neww > 0 && newh > 0 &&
		    (canvas.width !== neww || canvas.height !== newh)) {
			canvas.width  = neww;
			canvas.height = newh;
			lastConstraint = null;
			fitCanvasToParent();
		}

		// [10-15] per-client camera position: cam_x, cam_y, cam_z (int16_t LE)
		var rawCx = data[10] | (data[11] << 8);
		if (rawCx & 0x8000) rawCx -= 0x10000;
		var rawCy = data[12] | (data[13] << 8);
		if (rawCy & 0x8000) rawCy -= 0x10000;
		var rawCz = data[14] | (data[15] << 8);
		if (rawCz & 0x8000) rawCz -= 0x10000;
		camX = rawCx; camY = rawCy; camZ = rawCz;
		updateCameraHud();

		// [16] nick length
		var nickSize = data[16];
		// [17..16+nickSize] active player nick
		var activeNick = "";
		for (var i = 17; (i < 17 + nickSize) && data[i] !== 0; i++) {
			activeNick += String.fromCharCode(data[i]);
		}
		gameStatus.currentPlayer = decodeURIComponent(activeNick);

		renderQueueStatus(gameStatus);
		// Tile data starts at 17+nickSize
		renderUpdate(ctx, data, nickSize+17, graphicsMode);

		var now = performance.now();
		var nextFrame = (1000 / MAX_FPS) - (now - lastFrame);
		if (nextFrame < 4) {
			websocket.send(new Uint8Array([cmd.update]));
		} else {
			setTimeout(function() {
				websocket.send(new Uint8Array([cmd.update]));
			}, nextFrame);
		}
		lastFrame = performance.now();
		if (stats) { stats.end(); }
	} else if (data[0] === cmd.cursorsUpdate) {
		// Opcode 112: ghost cursor broadcast
		// Format: [112, count, (nick_len, nick..., tile_x, tile_y, color_idx)...]
		var cursorCanvas = document.getElementById('cursorCanvas');
		if (cursorCanvas) {
			// Match size to main canvas
			if (cursorCanvas.width !== canvas.width || cursorCanvas.height !== canvas.height) {
				cursorCanvas.width  = canvas.width;
				cursorCanvas.height = canvas.height;
			}
			var cctx = cursorCanvas.getContext('2d');
			cctx.clearRect(0, 0, cursorCanvas.width, cursorCanvas.height);
			var count = data[1];
			// Cursor colors by slot index: 0=driver(white), 1=cyan, 2=yellow, 3=magenta...
			var CURSOR_COLORS = ['#ffffff', '#00e5ff', '#ffea00', '#ff40ff', '#40ff80'];
			var offset = 2;
			for (var ci = 0; ci < count; ci++) {
				var nick_len = data[offset++];
				var pnick = '';
				for (var ni = 0; ni < nick_len - 1 && offset + ni < data.length; ni++) {
					if (data[offset + ni] === 0) break;
					pnick += String.fromCharCode(data[offset + ni]);
				}
				offset += nick_len;
				var cx = data[offset++];
				var cy = data[offset++];
				var cidx = data[offset++];
				var color = CURSOR_COLORS[cidx % CURSOR_COLORS.length];
				// Draw 1-tile border highlight
				cctx.strokeStyle = color;
				cctx.lineWidth = 2;
				cctx.strokeRect(cx * tilew + 1, cy * tileh + 1, tilew - 2, tileh - 2);
				// Draw nick label above
				if (pnick) {
					cctx.fillStyle = color;
					cctx.font = '9px monospace';
					cctx.fillText(pnick, cx * tilew, cy * tileh - 2);
				}
			}
		}
	} else if (data[0] === cmd.mapInfo && data.length >= 7) {
		// Opcode 118: map dimensions in tiles.
		// Format: [118, map_x lo, hi, map_y lo, hi, map_z lo, hi]
		mapW = data[1] | (data[2] << 8);
		mapH = data[3] | (data[4] << 8);
		mapD = data[5] | (data[6] << 8);
	}
}

function colorize(img, cnv) {
	var ctx3 = cnv.getContext('2d');

	for (var j = 0; j < 4; j++) {
		for (var i = 0; i < 4; i++) {
			var c = j * 4 + i;

			ctx3.drawImage(img, i * 256, j * 256);

			var idata = ctx3.getImageData(i * 256, j * 256, 256, 256);
			var pixels = idata.data;

			for (var u = 0, len = pixels.length; u < len; u += 4) {
				// DF's curses_* tilesheets use magenta (255,0,255) as the
				// transparent color-key (classic bitmap convention, no
				// alpha channel). Strip it before tinting.
				if (pixels[u] === 255 && pixels[u + 1] === 0 && pixels[u + 2] === 255) {
					pixels[u + 3] = 0;
					continue;
				}
				pixels[u]     = pixels[u]     * (colors[c * 3 + 0] / 255);
				pixels[u + 1] = pixels[u + 1] * (colors[c * 3 + 1] / 255);
				pixels[u + 2] = pixels[u + 2] * (colors[c * 3 + 2] / 255);
			}
			ctx3.putImageData(idata, i * 256, j * 256);

			ctx3.fillStyle = 'rgb(' +
					colors[c * 3 + 0] + ',' +
					colors[c * 3 + 1] + ',' +
					colors[c * 3 + 2] + ')';

			ctx3.fillRect(i * 256 + 16 * 15, j * 256 + 16 * 15, 16, 16);
		}
	}
}

// Returns a loader callback; calls init() when all pending loads complete.
var make_loader = function() {
	var loading = 0;
	return function() {
		loading += 1;
		return function() {
			loading -= 1;
			if (loading <= 0) {
				init();
			}
		};
	};
}();

var cd, ct;
function init() {
	document.body.style.backgroundColor =
		'rgb(' + colors[0] + ',' + colors[1] + ',' + colors[2] + ')';

	cd = document.createElement('canvas');
	cd.width = cd.height = 1024;
	colorize(ts, cd);

	ct = document.createElement('canvas');
	ct.width = ct.height = 1024;
	colorize(tt, ct);

	lastFrame = performance.now();

	connect();
}

var stats;
if (params.show_fps) {
	stats = new Stats();
	document.body.appendChild(stats.domElement);
	stats.domElement.style.position = "absolute";
	stats.domElement.style.bottom = "0";
	stats.domElement.style.left   = "0";
}

function getFolder(path) {
	return path.substring(0, path.lastIndexOf('/') + 1);
}

var root = getFolder(window.location.pathname);

var ts = document.createElement('img');
ts.src =  root + "art/" + tileSet;
ts.onload = make_loader();

var tt = document.createElement('img');
tt.src = root + "art/" + textSet;
tt.onload = make_loader();

if (colorscheme !== undefined) {
	var colorReq = new XMLHttpRequest();
	var colorLoader = make_loader();
	colorReq.onload = function() {
		colors = JSON.parse(this.responseText);
		colorLoader();
	};
	colorReq.open("get", root + "colors/" + colorscheme);
	colorReq.send();
}


var canvas = document.getElementById('myCanvas');

document.onkeydown = function(ev) {
	if (ev.keyCode === 91 ||
	    ev.keyCode === 18 ||
	    ev.keyCode === 17 ||
	    ev.keyCode === 16) {
		return;
	}

	if (ev.keyCode < 65) {
		// Non-alpha key: only the focus player sends game input.
		if (!active) { ev.preventDefault(); return; }
		var mod = (ev.shiftKey << 1) | (ev.ctrlKey << 2) | ev.altKey;
		var data = new Uint8Array([cmd.sendKey, ev.keyCode, 0, mod]);
		websocket.send(data);
		ev.preventDefault();
	} else {
		// ignore: alpha keys are handled by onkeypress
	}
};

document.onkeypress = function(ev) {
	var ch = ev.charCode;
	// WASD camera routing.
	// Route to camera when: we are NOT the active player, OR we are the
	// active player but no menu/dialog is open.
	var routeToCamera = !active || !isInMenu;
	if (routeToCamera && (ch === 119 || ch === 87)) { // w/W — pan up
		sendCamMove(0, -10, 0); ev.preventDefault(); return;
	}
	if (routeToCamera && (ch === 115 || ch === 83)) { // s/S — pan down
		sendCamMove(0,  10, 0); ev.preventDefault(); return;
	}
	if (routeToCamera && (ch === 97  || ch === 65)) { // a/A — pan left
		sendCamMove(-10, 0, 0); ev.preventDefault(); return;
	}
	if (routeToCamera && (ch === 100 || ch === 68)) { // d/D — pan right
		sendCamMove( 10, 0, 0); ev.preventDefault(); return;
	}

	// Only the focus player can send game input.
	if (!active) { ev.preventDefault(); return; }

	var mod = (ev.shiftKey << 1) | (ev.ctrlKey << 2) | ev.altKey;
	var data = new Uint8Array([cmd.sendKey, 0, ev.charCode, mod]);
	websocket.send(data);

	if (ev.stopPropagation) {
		ev.stopPropagation();
	} else if (window.event) {
		window.event.cancelBubble = true;
	}
	ev.preventDefault();
};


var lastConstraint = null;
function fitCanvasToParent() {
	// DF itself stretches its square tiles to fit the window (non-square
	// cells), so mirror that: just fill the container on both axes.
	canvas.style.width  = "100%";
	canvas.style.height = "100%";
	// Keep the cursor overlay canvas exactly on top of the main canvas.
	var cursorCanvas = document.getElementById('cursorCanvas');
	if (cursorCanvas) {
		cursorCanvas.style.width  = canvas.style.width;
		cursorCanvas.style.height = canvas.style.height;
	}
}

// Update the camera position HUD overlay (if present in the HTML).
function updateCameraHud() {
	var hud = document.getElementById('cameraHud');
	if (hud) {
		hud.textContent = 'X:' + camX + ' Y:' + camY + ' Z:' + camZ;
	}
}

// Mouse opcode type constants (must match server.cpp / webfort.cpp)
var MOUSE_MOVE  = 0;
var MOUSE_DOWN  = 1;
var MOUSE_UP    = 2;
var MOUSE_WHEEL = 3;

// Translate a canvas pointer event to tile coordinates.
function canvasToTile(ev) {
	var rect = canvas.getBoundingClientRect();
	var scaleX = canvas.width  / rect.width;
	var scaleY = canvas.height / rect.height;
	var px = (ev.clientX - rect.left) * scaleX;
	var py = (ev.clientY - rect.top)  * scaleY;
	var tx = Math.max(0, Math.min(dimx - 1, Math.floor(px / tilew)));
	var ty = Math.max(0, Math.min(dimy - 1, Math.floor(py / tileh)));
	return [tx, ty];
}

function sendMouseEvent(type, tile, button) {
	if (!websocket || websocket.readyState !== WebSocket.OPEN) return;
	var data = new Uint8Array([cmd.sendMouse, tile[0], tile[1], button, type]);
	websocket.send(data);
}

// Throttle mousemove to ~60Hz.
var lastMouseSend = 0;
canvas.addEventListener('mousemove', function(ev) {
	var now = Date.now();
	if (now - lastMouseSend < 16) return;
	lastMouseSend = now;
	var tile = canvasToTile(ev);
	sendMouseEvent(MOUSE_MOVE, tile, 0);
	// Send ghost cursor only when NOT the active player.
	// When active, the cursor is already tracked server-side via gps->mouse_x/y
	// (opcode 113). Sending opcode 114 while active causes the stale cursor_active
	// flag to linger after losing focus, producing a frozen ghost cursor.
	if (!active && websocket && websocket.readyState === WebSocket.OPEN) {
		websocket.send(new Uint8Array([cmd.cursorMove, tile[0], tile[1]]));
	}
	ev.preventDefault();
});

canvas.addEventListener('mousedown', function(ev) {
	var btn = ev.button === 2 ? 2 : (ev.button === 1 ? 3 : 1);
	sendMouseEvent(MOUSE_DOWN, canvasToTile(ev), btn);
	ev.preventDefault();
});

canvas.addEventListener('mouseup', function(ev) {
	var btn = ev.button === 2 ? 2 : (ev.button === 1 ? 3 : 1);
	sendMouseEvent(MOUSE_UP, canvasToTile(ev), btn);
	ev.preventDefault();
});

canvas.addEventListener('wheel', function(ev) {
	// Context-aware scroll routing:
	// Focus player + menu open  → game scroll (opcode 113 MOUSE_WHEEL)
	// All other cases           → camera Z movement (opcode 117 CamMove)
	if (active && isInMenu) {
		var btn = ev.deltaY > 0 ? 5 : 4; // 4=scroll-up 5=scroll-down
		sendMouseEvent(MOUSE_WHEEL, canvasToTile(ev), btn);
	} else {
		// Scroll up (deltaY < 0) = move toward sky (higher z in DF).
		var dz = ev.deltaY < 0 ? 1 : -1;
		sendCamMove(0, 0, dz);
	}
	ev.preventDefault();
}, { passive: false });

canvas.addEventListener('contextmenu', function(ev) {
	ev.preventDefault();
});

window.onresize = fitCanvasToParent;
window.onload   = fitCanvasToParent;
