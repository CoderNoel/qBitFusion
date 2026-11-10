# Security Policy

## Reporting a vulnerability

Please do not report security problems in public issues.

Use GitHub's private vulnerability reporting for this repository: open the **Security** tab and choose
**Report a vulnerability**. Only the maintainers can see the report.

Include, as far as you can:

1. the kind of problem and the part of the program it affects;
2. step-by-step instructions to reproduce it;
3. a proof of concept, if you have one;
4. what an attacker could achieve.

Never include real provider tokens, signed download links or other credentials in a report. If a
credential of yours was exposed while you investigated, revoke it with the provider.

This is a volunteer project, so there is no guaranteed response time. Reports are handled as fast as the
maintainers can manage, and fixes are credited to the reporter unless they prefer otherwise.

## Scope

In scope is the code this project adds to qBittorrent, in particular:

- the hybrid transport (`src/base/hybrid`): response validation, the URL guard, credential storage, and
  anything that could write unverified data to disk;
- anything that makes a "Debrid only" torrent contact trackers, DHT or peers;
- anything that puts a token or a signed link into a log, a settings file or an export.

A vulnerability that is also present in stock qBittorrent should be reported to the
[qBittorrent project](https://github.com/qbittorrent/qBittorrent/security/policy), which can fix it for
everyone. Feel free to tell us as well, so that the fix is merged here quickly.

## Supported versions

Only the latest commit on the default branch is supported. There are no maintained release branches yet.
