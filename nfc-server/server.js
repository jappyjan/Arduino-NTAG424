const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');
const fs = require('fs');
const url = require('url'); // Added for parsing WebSocket URL

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

const PORT = process.env.PORT || 3000;

// --- Configuration ---
const KEYS_FILE = path.join(__dirname, 'keys.json');
let keys = {};

function loadKeys() {
    try {
        if (fs.existsSync(KEYS_FILE)) {
            const data = fs.readFileSync(KEYS_FILE, 'utf8');
            keys = JSON.parse(data);
            console.log('Keys loaded successfully from', KEYS_FILE);
            if (keys.WARNING) {
                console.warn(`\n*** ${keys.WARNING} ***\n`);
            }
        } else {
            console.error('Error: keys.json not found!');
            console.error('Please create a keys.json file based on the example.');
            // Create a dummy keys object to prevent crashes
            keys = {
                WARNING: "keys.json was missing, using dummy keys.",
                defaultKey: "00".repeat(16),
                masterKey: "11".repeat(16),
                authKey: "AA".repeat(16),
                readKey: "BB".repeat(16), // Added examples for completeness
                writeKey: "CC".repeat(16),
                changeKey: "DD".repeat(16)
            };
             fs.writeFileSync(KEYS_FILE, JSON.stringify(keys, null, 2));
             console.log('Created dummy keys.json. Please review and update it!');
        }
    } catch (err) {
        console.error('Error loading or parsing keys.json:', err);
        process.exit(1); // Exit if keys can't be loaded
    }
}

// --- State Management ---
// Reader States: WAITING_FOR_READER, WAITING_FOR_CARD, CARD_PRESENT
let readerState = { // Only supporting one reader for now
    readerStatus: 'WAITING_FOR_READER',
    currentCardUid: null,
    lastSeen: null,
    // commandQueued: null // No longer needed, commands sent directly
    readerConnected: false, // Track reader WebSocket connection
};
let readerWs = null; // Store the reader's WebSocket connection

let logHistory = []; // Store recent logs
const MAX_LOG_HISTORY = 100;

// --- Helper Functions ---
function addLog(message) {
    const timestampedMessage = `[${new Date().toISOString()}] ${message}`;
    console.log(timestampedMessage); // Log to console with timestamp
    logHistory.push(timestampedMessage);
    if (logHistory.length > MAX_LOG_HISTORY) {
        logHistory.shift(); // Remove oldest log
    }
    broadcast({ type: 'log', data: timestampedMessage });
}

// Broadcast to UI clients only
function broadcastToUI(message) {
    const messageString = JSON.stringify(message);
    wss.clients.forEach(client => {
        // Send only to UI clients (not the reader)
        if (client !== readerWs && client.readyState === WebSocket.OPEN) {
             try {
                 client.send(messageString);
             } catch (error) {
                  addLog(`[Server] Error sending message to UI client: ${error.message}`);
             }
        }
    });
}


function broadcast(message) {
    broadcastToUI(message); // Keep old name but route to UI only broadcast
}

function broadcastStatusUpdate() {
    broadcastToUI({ type: 'status_update', data: readerState });
}

// Simple hex string to byte array helper
function hexToBytes(hex) {
    if (!hex || typeof hex !== 'string' || hex.length % 2 !== 0) {
        addLog(`[Server] Invalid hex string provided: ${hex}`);
        return null;
    }
    let bytes = [];
    for (let i = 0; i < hex.length; i += 2) {
        const byte = parseInt(hex.substr(i, 2), 16);
         if (isNaN(byte)) {
             addLog(`[Server] Invalid hex character in string: ${hex}`);
             return null;
         }
        bytes.push(byte);
    }
    return bytes;
}

// --- Middleware ---
app.use(express.json()); // Parse JSON request bodies - Keep for potential future use, but not for WS
app.use(express.static(path.join(__dirname, 'public'))); // Serve static files

// --- REMOVED HTTP API Endpoints ---
// app.post('/api/card_present', ...);
// app.get('/api/get_command/:uid', ...);
// app.post('/api/command_result', ...);


// --- WebSocket Communication ---

// Log raw upgrade requests to see if the ESP32 is even hitting the server
server.on('upgrade', (request, socket, head) => {
  const pathname = url.parse(request.url).pathname;
  addLog(`[Server] Received HTTP upgrade request for path: ${pathname} from ${request.socket.remoteAddress}`);
  // Log headers from the upgrade request
  addLog(`[Server] Upgrade Request Headers: ${JSON.stringify(request.headers, null, 2)}`);

  // You could potentially deny the upgrade here based on path or headers
  // if (pathname !== '/reader' && pathname !== '/ui') {
  //   addLog(`[Server] Denying upgrade for unknown path: ${pathname}`);
  //   socket.destroy();
  //   return;
  // }

  // Note: The 'ws' library handles the actual upgrade after this event if the path matches its config.
  // We don't need to explicitly call wss.handleUpgrade here unless using 'noServer: true' mode.
});

wss.on('connection', (ws, req) => {
    const location = url.parse(req.url, true);
    const clientType = location.pathname; // e.g., '/ui' or '/reader'
    const clientIp = req.socket.remoteAddress;

    // Log headers upon successful connection
    addLog(`[Server] WebSocket connection established for path: ${clientType} from ${clientIp}`);
    addLog(`[Server] Connection Headers: ${JSON.stringify(req.headers, null, 2)}`);

    if (clientType === '/reader') {
        // If a readerWs object already exists, assume it's the previous connection.
        // Terminate it forcefully to prevent race conditions with the 'close' event.
        if (readerWs) {
            addLog(`[Server] Existing readerWs found while handling new connection from ${clientIp}. Terminating old connection.`);
            try {
                readerWs.terminate(); // Force close immediately
            } catch (e) {
                addLog(`[Server] Error terminating old readerWs: ${e.message}`);
            }
            // Explicitly nullify here, don't wait for the close event which might be delayed
            readerWs = null;
            // Reset state immediately as well
            readerState.readerConnected = false;
            readerState.readerStatus = 'WAITING_FOR_READER';
            readerState.currentCardUid = null;
        }

        // Accept the new connection
        readerWs = ws;
        readerState.readerConnected = true;
        readerState.readerStatus = 'WAITING_FOR_CARD'; // Assume ready once connected
        readerState.currentCardUid = null; // Reset card state on new connection
        addLog('[Server] NFC Reader connected via WebSocket');
        broadcastStatusUpdate();

        ws.on('message', (message) => {
            let parsedMessage;
            try {
                parsedMessage = JSON.parse(message);
                // addLog(`[Server] Message from Reader: ${message}`); // Can be noisy
            } catch (error) {
                addLog(`[Server] Received invalid JSON from Reader: ${message}`);
                return;
            }

            // Handle messages FROM the reader
            switch (parsedMessage.type) {
                case 'status':
                    handleReaderStatus(parsedMessage.payload);
                    break;
                case 'command_result':
                    handleReaderCommandResult(parsedMessage.payload);
                    break;
                 case 'log': // Allow reader to send logs directly
                      if (parsedMessage.payload && typeof parsedMessage.payload === 'string') {
                         addLog(`[Reader] ${parsedMessage.payload}`);
                      }
                     break;
                default:
                    addLog(`[Server] Received unknown message type from Reader: ${parsedMessage.type}`);
            }
        });

        ws.on('close', () => {
            addLog('[Server] NFC Reader disconnected');
            readerWs = null;
            readerState.readerConnected = false;
            readerState.readerStatus = 'WAITING_FOR_READER';
            readerState.currentCardUid = null;
            broadcastStatusUpdate();
        });

        ws.on('error', (error) => {
            addLog(`[Server] Reader WebSocket error: ${error.message}`);
            // Close event will handle cleanup
        });

    } else { // Assume UI client if not '/reader'
        addLog(`[Server] UI Client connected via WebSocket from ${clientIp}`);

        // Send current state and log history to the new UI client
        ws.send(JSON.stringify({ type: 'status_update', data: readerState }));
        ws.send(JSON.stringify({ type: 'log_history', data: logHistory }));

        ws.on('message', (message) => {
            let parsedMessage;
            try {
                parsedMessage = JSON.parse(message);
                // addLog(`[Server] Message from UI: ${message}`); // Can be noisy
            } catch (error) {
                addLog(`[Server] Received invalid JSON from UI: ${message}`);
                return;
            }

            // Handle messages FROM the UI (Commands)
            if (parsedMessage.type === 'command') {
                handleUICommand(parsedMessage.command, ws); // Pass ws to send error back if needed
            } else {
                addLog(`[Server] Received unknown message type from UI: ${parsedMessage.type}`);
            }
        });

        ws.on('close', () => {
            addLog('[Server] UI Client disconnected');
        });

        ws.on('error', (error) => {
            addLog(`[Server] UI WebSocket error: ${error.message}`);
        });
    }
});

// --- WebSocket Message Handlers ---

function handleReaderStatus(payload) {
    const { uid } = payload; // readerId could be added later
    // addLog(`[Server] Received status from reader. UID: ${uid || 'None'}`);

    let statusChanged = false;

    // Reader connection is handled by ws.on('connection')/('close')
    // We only need to update card status here

    if (uid) { // Card is present
        if (readerState.currentCardUid !== uid) {
            readerState.currentCardUid = uid;
            readerState.readerStatus = 'CARD_PRESENT';
            addLog(`[Server] Card presented: ${uid}`);
            statusChanged = true;
        }
        readerState.lastSeen = Date.now(); // Update last seen time regardless
    } else { // No card present
        if (readerState.currentCardUid !== null) {
            addLog(`[Server] Card removed: ${readerState.currentCardUid}`);
            readerState.currentCardUid = null;
            readerState.readerStatus = 'WAITING_FOR_CARD';
            statusChanged = true;
        }
         // If reader says no card, ensure status reflects waiting
         if (readerState.readerStatus !== 'WAITING_FOR_CARD' && readerState.readerConnected) {
              readerState.readerStatus = 'WAITING_FOR_CARD';
              statusChanged = true;
         }
    }

    if (statusChanged) {
        broadcastStatusUpdate();
    }
}

function handleReaderCommandResult(payload) {
    const { uid, success, message, logs } = payload;
    addLog(`[Server] Received command result for UID: ${uid}, Success: ${success}`);
    if (message) {
        addLog(`[Server] Result Message: ${message}`);
    }
    if (logs && Array.isArray(logs)) {
        logs.forEach(log => addLog(`[Reader] ${log}`));
    } else if (logs) {
         addLog(`[Reader] ${logs}`); // Handle single log string
    }

    // Send result details to UI clients
    broadcastToUI({ type: 'command_result', data: { uid, success, message } });

    // No need to update readerState based on result for now
}

function handleUICommand(command, uiWs) {
     const sendErrorToUI = (msg) => {
         addLog(`[Server] Error queuing command: ${msg}`);
         if (uiWs && uiWs.readyState === WebSocket.OPEN) {
              uiWs.send(JSON.stringify({type: 'log', data: `[Server] Error: ${msg}`}));
              // Optionally send a specific error type message
              uiWs.send(JSON.stringify({type: 'command_error', data: { message: msg }}));
         }
     };

    if (!readerWs || readerWs.readyState !== WebSocket.OPEN) {
        sendErrorToUI('Reader not connected.');
        return;
    }

    if (readerState.readerStatus !== 'CARD_PRESENT' || !readerState.currentCardUid) {
        sendErrorToUI('No card present to execute command.');
        return;
    }

    // Basic check if a command might still be executing (though ESP should handle one at a time)
    // We rely on the UI disabling buttons while waiting for 'command_result'
    // addLog(`[Server] Received command '${command}' from UI for UID: ${readerState.currentCardUid}`);

    const uid = readerState.currentCardUid;
    let commandData = { command, uid }; // Base command structure for ESP

    // Add necessary keys based on command
    // WARNING: Sending keys like this is insecure!
    if (command === 'enroll') {
        // Ensure all required keys exist in the loaded keys config
        const requiredKeys = ['masterKey', 'authKey', 'readKey', 'writeKey', 'changeKey', 'defaultKey'];
        let missingKeys = [];
        let invalidKeys = [];
        let keyBytes = {};

        for (const keyName of requiredKeys) {
            if (!keys[keyName]) {
                missingKeys.push(keyName);
                continue;
            }
            const bytes = hexToBytes(keys[keyName]);
            if (!bytes || bytes.length !== 16) { // NTAG keys are 16 bytes
                invalidKeys.push(keyName);
            } else {
                 keyBytes[keyName] = bytes; // Store the byte array
            }
        }

        if (missingKeys.length > 0) {
             sendErrorToUI(`Missing keys in keys.json for enrollment: ${missingKeys.join(', ')}`);
             return;
        }
        if (invalidKeys.length > 0) {
            sendErrorToUI(`Invalid hex format or length (must be 32 hex chars / 16 bytes) for keys: ${invalidKeys.join(', ')}`);
            return;
        }
         commandData.keys = keyBytes; // Add the validated byte arrays

    } else if (command === 'authenticate') {
        if (!keys.authKey) {
             sendErrorToUI("Missing 'authKey' in keys.json for authentication.");
             return;
        }
        const authKeyBytes = hexToBytes(keys.authKey);
         if (!authKeyBytes || authKeyBytes.length !== 16) {
              sendErrorToUI("Invalid hex format or length for 'authKey' (must be 32 hex chars / 16 bytes).");
              return;
         }
        commandData.keyNo = 1; // Key number to use for authentication (e.g., App Key 1)
        commandData.authKey = authKeyBytes; // Send the specific key needed (as byte array)

    } else {
        sendErrorToUI(`Unknown command received: ${command}`);
        return;
    }

    // Send the command to the reader WebSocket
    try {
        const commandString = JSON.stringify({ type: 'command', payload: commandData });
        readerWs.send(commandString);
        addLog(`[Server] Sent command '${command}' to reader for UID: ${uid}`);
        // Notify UI that command was sent (UI uses this to disable buttons)
        broadcastToUI({ type: 'command_sent', data: { command: command } });
    } catch (error) {
         sendErrorToUI(`Failed to send command to reader: ${error.message}`);
         // Clean up readerWs if send failed? Potentially overkill.
    }
}


// --- Server Start ---
server.listen(PORT, () => {
    loadKeys(); // Load keys before starting
    console.log(`\nServer listening on http://localhost:${PORT}`);
    addLog('[Server] Server started. Waiting for NFC Reader and UI connections via WebSocket...'); // Updated log message
    broadcastStatusUpdate(); // Send initial status to any connecting UI
});