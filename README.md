# cardputer-telegram-messenger

A text messenger for the M5Cardputer that sends and receives Telegram messages. It has on-device Wi-Fi setup, per-contact chat history, and a contact list, all without a phone or computer once it's set up.

## Features

- Scan and join Wi-Fi networks from the device (saved to flash)
- Telegram contacts with per-contact chat history saved to flash
- Messages from new people are added to the contact list automatically
- Scroll chat history with Fn + arrows

## Important: use your own bot

This project does not come with a bot or token. **Every user must create their own Telegram bot and use their own token.** Do not use anyone else's token, and never commit your token to GitHub.

## Hardware

- M5Stack Cardputer (ESP32-S3)
- A 2.4 GHz Wi-Fi network (the ESP32 can't join 5 GHz-only networks)

## Setup

### 1. Create your Telegram bot

1. Open Telegram and message **@BotFather**.
2. Send `/newbot`, then follow the prompts to choose a name and username.
3. BotFather replies with a token that looks like `123456789:AAH...`. Keep it private.

### 2. Get your chat ID

1. Message **@userinfobot** in Telegram.
2. It replies with your numeric ID, such as `123456789`. Use this number, not your username.

### 3. Start the bot

Open your new bot in Telegram and tap **Start** (or send `/start`). Telegram won't let a bot message you until you do this.

Anyone you want to text must also open **your** bot and press Start. Each person needs to do this with the same bot.

### 4. Install the Arduino IDE software

1. Install the Arduino IDE 2.x.
2. Add the M5Stack board package: **File → Preferences → Additional boards manager URLs** → add `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`, then install **M5Stack** from the Boards Manager.
3. Install the **M5Cardputer** library and **ArduinoJson v7** from the Library Manager.
4. Under **Tools**, set the partition scheme to one that includes a file system, such as **Default 4MB with spiffs**. Chat history needs it.

### 5. Add your token and upload

1. Open `cardputer_messenger/cardputer_messenger.ino`.
2. Replace `YOUR_BOT_TOKEN_HERE` with your bot token. Keep the quotes.
3. Select the Cardputer board and its port, then upload.

## First use

1. Press **G0** (the button on the top side) to open the **WiFi** screen. It scans automatically.
2. Use `;` and `.` to move, and press **Enter** to join a network. Enter the password when asked.
3. Press **G0** again to return to the chat screen.
4. Press **G0** once more from the chat screen to open **Contacts**. Press **a** to add a contact: type a name, press Enter, type the numeric chat ID, and press Enter.
5. Go back to the chat screen with Enter on a contact and start typing.

## Controls

| Screen | Keys |
|---|---|
| Chat | Type, **Enter** send, **Backspace** delete, **Fn + ;** / **Fn + .** scroll history |
| Contacts | `;` / `.` move, **Enter** open chat, **a** add, **x** delete (with history) |
| WiFi | `;` / `.` move, **Enter** join, **r** rescan, **x** forget saved network |
| Any screen | **G0** cycles: Chat → Contacts → WiFi → Chat |

Saved contacts, chat history, and Wi-Fi networks are kept across reboots.

## Troubleshooting

- **"Send failed: 400":** the chat ID is wrong, or the person hasn't pressed Start on your bot.
- **"Send failed: 403":** the bot is blocked or the person hasn't started it.
- **"No WiFi":** check the password and that the network is 2.4 GHz.
- **Compile error "expected '}'":** the paste was incomplete. Paste the full file again.
- **Serial Monitor (115200 baud):** shows boot messages and the result code for each send.

## Security

- Bot tokens and Wi-Fi passwords are stored in plain text on the device. Only use it on devices you control.
- Don't commit tokens, passwords, or chat IDs to GitHub. Check your code before every push.
- If your token is exposed, revoke it in @BotFather with `/revoke`, then update your sketch.
- Telegram bots only see messages sent to them directly. They don't create group chats between your contacts.

## License

Add a license of your choice (for example MIT) before publishing.
