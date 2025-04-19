document.addEventListener('DOMContentLoaded', () => {
    const readerStatusElem = document.getElementById('reader-status');
    const cardStatusElem = document.getElementById('card-status');
    const controlsContainer = document.getElementById('controls-container');
    const enrollButton = document.getElementById('enroll-button');
    const authenticateButton = document.getElementById('authenticate-button');
    const commandStatusElem = document.getElementById('command-status');
    const logOutputElem = document.getElementById('log-output');

    let socket;
    let currentUid = null;
    let buttonsEnabled = false;
    let commandPending = false;

    function connectWebSocket() {
        const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        socket = new WebSocket(`${wsProtocol}//${window.location.host}/ui`);

        socket.onopen = () => {
            console.log('WebSocket connection established');
            readerStatusElem.textContent = 'Connected to server. Waiting for reader...';
            readerStatusElem.className = 'status-waiting-reader';
            appendLog('[UI] WebSocket connection established', 'log-server');
        };

        socket.onmessage = (event) => {
            try {
                const message = JSON.parse(event.data);
                console.log('Message from server:', message);
                handleServerMessage(message);
            } catch (error) {
                console.error('Failed to parse message or invalid message format:', event.data, error);
                appendLog(`[UI] Error processing message: ${event.data}`, 'log-error');
            }
        };

        socket.onerror = (error) => {
            console.error('WebSocket error:', error);
            readerStatusElem.textContent = 'WebSocket connection error.';
            readerStatusElem.className = '';
            appendLog('[UI] WebSocket error', 'log-error');
            disableControls();
        };

        socket.onclose = () => {
            console.log('WebSocket connection closed. Attempting to reconnect...');
            readerStatusElem.textContent = 'WebSocket connection closed. Retrying...';
            readerStatusElem.className = '';
            appendLog('[UI] WebSocket closed. Reconnecting...', 'log-server');
            disableControls();
            currentUid = null;
            // Attempt to reconnect after a delay
            setTimeout(connectWebSocket, 5000);
        };
    }

    function handleServerMessage(message) {
        switch (message.type) {
            case 'status_update':
                updateStatus(message.data);
                break;
            case 'command_sent':
                // Server confirms command was sent to reader
                commandPending = true;
                updateButtonStates(); // Disable buttons
                commandStatusElem.textContent = 'Command sent to reader, waiting for result...';
                commandStatusElem.className = 'status-pending'; // Optional: Add CSS for pending
                appendLog(`[UI] Server acknowledged sending command: ${message.data.command}`, 'log-server');
                break;
            case 'log':
            case 'command_result':
                if (message.type === 'log') {
                    const logClass = message.data.startsWith('[Server]') ? 'log-server' : message.data.startsWith('[Reader]') ? 'log-reader' : '';
                    appendLog(message.data, logClass);
                } else { // command_result
                     // Process result data
                    const { uid, success, message: resultMsg } = message.data;
                    const resultText = `Last command: ${success ? 'Success' : 'Failed'}. ${resultMsg || ''}`;
                    const logText = `[Result] UID: ${uid}, Success: ${success}, Msg: ${resultMsg}`;

                    // Log the result
                    // Consider adding log-success/log-error classes to your CSS for styling
                    appendLog(logText, success ? 'log-success' : 'log-error');

                    // Update command status element
                    commandStatusElem.textContent = "Last Result: " + resultText.trim();
                    commandStatusElem.className = success ? 'status-success' : 'status-error';

                    // Update command pending state and buttons
                    commandPending = false;
                    updateButtonStates();
                }
                break;
            case 'log_history':
                 logOutputElem.textContent = ''; // Clear existing logs
                 message.data.forEach(log => {
                    const logClass = log.startsWith('[Server]') ? 'log-server' : log.startsWith('[Reader]') ? 'log-reader' : '';
                    appendLog(log, logClass, false); // Don't scroll for history
                 });
                 scrollToBottom(logOutputElem);
                 break;
            default:
                console.log('Unknown message type:', message.type);
                appendLog(`[UI] Unknown message type: ${message.type}`, 'log-error');
        }
    }

    function updateStatus(statusData) {
         commandPending = statusData.commandQueued || false;

        switch (statusData.readerStatus) {
            case 'WAITING_FOR_READER':
                readerStatusElem.textContent = 'Waiting for NFC Reader to connect...';
                readerStatusElem.className = 'status-waiting-reader';
                cardStatusElem.textContent = '';
                disableControls();
                currentUid = null;
                break;
            case 'WAITING_FOR_CARD':
                readerStatusElem.textContent = 'Reader connected. Waiting for card...';
                readerStatusElem.className = 'status-waiting-card';
                cardStatusElem.textContent = '';
                disableControls();
                currentUid = null;
                break;
            case 'CARD_PRESENT':
                readerStatusElem.textContent = 'Reader connected.';
                 readerStatusElem.className = 'status-card-present';
                currentUid = statusData.currentCardUid;
                cardStatusElem.textContent = `Card detected: UID ${currentUid}`;
                enableControls();
                break;
            default:
                 readerStatusElem.textContent = 'Unknown reader status.';
                 readerStatusElem.className = '';
                 cardStatusElem.textContent = '';
                 disableControls();
                 currentUid = null;
        }
         updateButtonStates();
    }

    function enableControls() {
        buttonsEnabled = true;
        updateButtonStates();
    }

    function disableControls() {
        buttonsEnabled = false;
        commandPending = false;
        updateButtonStates();
    }

     function updateButtonStates() {
        const enable = buttonsEnabled && !commandPending;
        enrollButton.disabled = !enable;
        authenticateButton.disabled = !enable;
        if (commandPending) {
            commandStatusElem.textContent = 'Command sent to reader, waiting for result...'; // Removed - only show final result
        }
    }

    function sendCommand(command) {
        if (socket && socket.readyState === WebSocket.OPEN && currentUid) {
            const message = JSON.stringify({ type: 'command', command: command });
            console.log('Sending command to server:', message);
            appendLog(`[UI] Sending command: ${command}`, 'log-server');

            socket.send(message);
            commandPending = true;
            updateButtonStates();
        } else {
            console.error('WebSocket not connected or no card present.');
            appendLog('[UI] Cannot send command: WebSocket not connected or no card present.', 'log-error');
        }
    }

    function appendLog(message, className = '', scroll = true) {
        const logEntry = document.createElement('div');
        logEntry.textContent = `[${new Date().toLocaleTimeString()}] ${message}`;
        if (className) {
            logEntry.classList.add(className);
        }
        logOutputElem.appendChild(logEntry);
        if (scroll) {
           scrollToBottom(logOutputElem);
        }
    }

    function scrollToBottom(element) {
         element.scrollTop = element.scrollHeight;
    }

    // --- Event Listeners ---
    enrollButton.addEventListener('click', () => sendCommand('enroll'));
    authenticateButton.addEventListener('click', () => sendCommand('authenticate'));

    // --- Initial Setup ---
    connectWebSocket();
}); 