const net = require('net');

class Client {
    constructor(socket, id) {
        this.socket = socket;
        this.id = id;
        this.buffer = '';
    }

    appendMessage(data) {
        this.buffer += data;
    }

    extractMessages() {
        let messages = this.buffer.split('\n');
        this.buffer = messages.pop();
        return messages;
    }
}

class Server {
    constructor(port) {
        this.port = port;
        this.clients = new Map();
        this.counter = 0;
        this.server = net.createServer((socket) => this.handleConnection(socket));
        this.server.on('error', (err) => console.error('Server error:', err));
    }

    handleConnection(socket) {
        const clientId = this.counter++;
        const client = new Client(socket, clientId);
        this.clients.set(clientId, client);

        console.log(`Client ${clientId} connected.`);
        this.broadcast(`server: client ${clientId} just arrived\n`, clientId);

        socket.on('data', (data) => this.handleData(client, data));
        socket.on('end', () => this.handleDisconnect(clientId));
        socket.on('error', (err) => console.error(`Client ${clientId} error:`, err));
    }

    handleData(client, data) {
        client.appendMessage(data.toString());
        const messages = client.extractMessages();
        for (const message of messages) {
            this.broadcast(`client ${client.id}: ${message}\n`, client.id);
        }
    }

    broadcast(message, senderId) {
        for (let [id, client] of this.clients) {
            if (id !== senderId) {
                client.socket.write(message);
            }
        }
    }

    handleDisconnect(clientId) {
        this.clients.delete(clientId);
        console.log(`Client ${clientId} disconnected.`);
        this.broadcast(`server: client ${clientId} just left\n`, clientId);
    }

    static start(port) {
        const serverInstance = new Server(port);
        serverInstance.server.listen(port, () => {
            console.log(`Server is listening on port ${port}`);
        });
    }
}

const PORT = process.argv[2] || 8000;
if (PORT <= 0 || PORT > 65535) {
    console.error("Invalid port number.");
    process.exit(1);
}

Server.start(PORT);
