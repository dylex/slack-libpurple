# slack-libpurple [![Build Status](https://travis-ci.org/dylex/slack-libpurple.svg?branch=master)](https://travis-ci.org/dylex/slack-libpurple)

A Slack protocol plugin for libpurple IM clients.

## Installation/Configuration

### Linux/macOS

1. Install libpurple (pidgin, finch, etc.), including necessary development components on binary distros (`libpurple-devel`, `libpurple-dev`, etc.);
1. Clone this repository with `git clone https://github.com/dylex/slack-libpurple.git`, run `cd slack-libpurple`, then run `sudo make install` or `make install-user`.

### Windows

@EionRobb is kindly providing windows builds [here](https://eion.robbmob.com/libslack.dll).

## Authentication

To login enter your email address as the `username` and
`<workspace>.slack.com` as the `host`.  You can also optionally enter your
password and have it saved.

For use with [Bitlbee](https://bitlbee.org) set `username` to `user@example.com%<workspace>.slack.com`.

### Legacy Authentication

Earlier versions of this plugin used Slack's legacy tokens for authentication.
This feature is still supported, but as Slack will be stopping the creation
of legacy tokens on May 5th 2020, it is highly advised that you migrate to the
username/password authentication from above.

1. [Issue a Slack API token](https://api.slack.com/custom-integrations/legacy-tokens) for yourself
1. Add your slack account to your libpurple program and enter this token under (Advanced) API token (do *not* enter your slack password; username/hostname are optional but can be set to `you@your.slack.com`)

If you're using a front-end (like Adium or Spectrum2) that does not let you set the API token, you can enter your token as the account password instead.

## Usage
Here's how slack concepts are mapped to purple:

   * Your "open" channels (on the slack bar) are mapped to the buddy list: joining a channel is equivalent to creating a buddy;
   * Which conversations are open in purple is up to you, and has no effect on slack;
   * For bitlbee IRC connections, Slack channels are "chat channels" that can be added to your configuration with "`chat add <account id> #<channel>`"

### Configuration options
- `api_token`: API token for legacy authentication
- `open_chat` [FALSE]: Open chat on channel message; open a chat window whenever there is activity in a channel
- `display_threads` [TRUE]: Display thread replies; display messages in a thread when they're posted
- `display_parent_indicator` [TRUE]: Re-display parent with indicator when thread is opened; the original messages will be displayed again when a thread is first created, follewd by  the threaded message
- `thread_indicator` [`⤷ `]: Prepend thread replies with this string
- `parent_indicator` [`◈ `]: Prepend parent messages with this string
- `thread_timestamp` [`%X`]: Thread timestamp format for the current day (time only), when the message is displayed the same day as it was posted
- `thread_datestamp` [`%x %X`]: Thread timestamp format for previous days (date and time), when the message is displayed on a different day than it was posted
- `connect_history` [FALSE]: Retrieve unread IM (and channel, if `open_history`) history on connect; opening any IMs that have new messages since they were last read, and also opening any channels with new activity if `open_history` is set
- `open_history` [FALSE]: Retrieve unread history on conversation open (and connect, if `connect_history`), displaying any messages since they were last read when you open a conversation
- `thread_history` [FALSE]: Retrieve unread thread history too (slow!); requires downloading the previous 1000 messages to check if any of them have new thread messages (we have yet to find a better way to check this through the slack API)
- `enable_avatar_download` [FALSE]: Download user avatars on connect
- `channel_members` [TRUE]: Show members in channels (disabling may break channel features)
- `attachment_prefix` [`▎ `]: Prepend attachment lines with this string
- `lazy_load` [FALSE]: Lazy loading: only request objects on demand (EXPERIMENTAL!); normally all users and conversations are loaded on connect, but with this option set, they are only loaded when they are seen. This requires an undocumented API call that shows only "active" conversations, like the slack web interface
- `ratelimit_delay` [15]: Seconds to delay when ratelimited; the slack API limits how many requests you can make how quickly. Normally it tells you how long you need to wait before making another call, but due to a parsing limitation in libpurple that we have not bothered to work around, we don't get this value, so have a hard-coded delay. Should only need to be changed in extreme circumstances, though it can also lead to longer delays than necessary.

### Available Commands
- `/history [count]`: fetch `count` (or unread, if not specified) previous messages
- `/edit [new message]`: edit your last message to be `new message`
- `/delete`: remove your last message
- `/thread|th [thread-timestamp] [message]`: post `message` in a thread, where `thread-timestamp` matches the configured display format (either `thread_timestamp` or `thread_datestamp`)
- `/getthread|gth [thread-timestamp]`: fetch messages in a thread, where `thread-timestamp` matches the configured display format (either `thread_timestamp` or `thread_datestamp`)
- `/call`: start a new call

## Known issues
- Handling of messages while not connected or not open is not great.
- 2FA and other authentication methods are not supported (#115).
- The author (@dylex) has only sporadic attention available and is often slow in responding, so community support and contributions are welcome and encouraged (much thanks to @EionRobb, @klali, @zoltan-dulac, @kacf, and others)!
