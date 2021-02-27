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

### Available Commands
- `/edit <new message>` will edit the last message you sent in a channel/chat to whatever you write. 
- `/delete` will delete the last message you sent in a channel/chat.

## Known issues
- Handling of messages while not connected or not open is not great.
- The `history` branch has some optional features for fetching history (thanks to @kacf) and dealing with large slacks (`lazy_load` option).
- Threads are only partially supported (see #118).
- 2FA and other authentication methods are not supported (#115).
- The author (@dylex) has only sporadic attention available and is often slow in responding, so community support and contributions are welcome and encouraged (much thanks to @EionRobb, @klali, @zoltan-dulac, @kacf, and others)!
