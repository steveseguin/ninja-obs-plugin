# Group room Guest Return screenshot walkthrough

Captured and visually reviewed on 2026-10-31 using real OBS 32.2.2 and Chrome. These images show the unreleased group-return controls built on plugin 1.1.77. See [validation and remaining work](../../docs/guest-return-room-validation.md) and the [setup guide](../../QUICKSTART.md#return-the-obs-program-to-a-group-room).

The cameras and tones are synthetic test media. The normal streaming destination is a local RTMP recorder standing in for YouTube. Room IDs and the password are disposable demo values; these sessions have ended. Do not reuse the shown links.

Each step has an original application/viewport capture and a crop. No UI was generated or repainted. [manifest.json](manifest.json) records crop coordinates, dimensions, SHA-256 hashes, binary provenance and individual review notes. [validation-results.json](validation-results.json) contains measured results. Steps are arranged for guide use, not capture timestamp order.

| Step | Guide crop | Original | Review |
| --- | --- | --- | --- |
| Keep the normal streaming destination | [View](cropped/01-main-destination.png) | [Full](full/01-main-destination.png) | Custom RTMP loopback receiver is the test stand-in for YouTube; stream key remains masked. Do not copy this server into a real production setup. |
| Select public audio track 1 | [View](cropped/02-main-audio-track.png) | [Full](full/02-main-audio-track.png) | Advanced output, track 1 selected, AAC for the public stream; separate return uses Opus. |
| Open the matching director room | [View](cropped/03-director-room.png) | [Full](full/03-director-room.png) | director room and invitation controls are visible. The plugin return is a room publisher; it does not claim the director role. |
| Bring individual guests into OBS | [View](cropped/04-guest-source.png) | [Full](full/04-guest-source.png) | individual view URL, room/password, solo and cleanoutput, and Control audio via OBS checked. Source preview is intentionally inactive in this post-session settings capture; crop focuses on settings. |
| Route audio for the whole room | [View](cropped/05-audio-routing.png) | [Full](full/05-audio-routing.png) | Alice, Ben and Casey are checked on track 1 and unchecked on track 2; host and clip are checked on both. The crop focuses on public track 1 and return track 2. This post-session settings capture preserves the tested routing. |
| Enter a room to enable the invitation | [View](cropped/06-room-required.png) | [Full](full/06-room-required.png) | empty Room ID disables Copy room guest invite, while the standalone viewer-link button remains available. |
| Configure the independent return | [View](cropped/07-return-settings.png) | [Full](full/07-return-settings.png) | room and stream ID are separate, password masked, track 2, x264, 2500 kbps and capacity 4. Both copy actions are visible. |
| Optional connection settings | [View](cropped/08-advanced-defaults.png) | [Full](full/08-advanced-defaults.png) | signaling, salt, ICE and Force TURN controls fit at normal width; default behavior and credential omission are explained. |
| Guest opens the copied invitation | [View](cropped/09-guest-invitation.png) | [Full](full/09-guest-invitation.png) | Join Room with Camera and Screenshare with Room are both visible; the guest invitation is not a viewing-only link. |
| Guest selects camera and microphone | [View](cropped/10-guest-devices.png) | [Full](full/10-guest-devices.png) | camera preview, START, audio source and output selector are visible. Fake devices are deliberate synthetic test inputs. |
| First guest receives the program | [View](cropped/11-first-guest.png) | [Full](full/11-first-guest.png) | OBS program title appears with guest self-preview. This was captured before individual camera sources were added to the program layout. |
| Check connected viewers and capacity | [View](cropped/12-return-capacity.png) | [Full](full/12-return-capacity.png) | 4 / 4 means three guests plus the director preview. Settings are disabled during the active return; copy and stop buttons remain enabled. |
| Director sees the return and all three guests | [View](cropped/13-director-full-room.png) | [Full](full/13-director-full-room.png) | distinct program return, Alice, Ben and Casey cards are visible. The return shows the OBS composition. |
| Alice sees the OBS program | [View](cropped/14-alice-program.png) | [Full](full/14-alice-program.png) | OBS title and all three composed cameras appear, with a separate self-preview. Ben and Casey appear in the audio-only participant list. |
| Ben sees the same program | [View](cropped/15-ben-program.png) | [Full](full/15-ben-program.png) | same OBS composition and a separate self-preview; other guests remain audio-only outside the program. |
| Casey joins late and receives the program | [View](cropped/16-casey-program.png) | [Full](full/16-casey-program.png) | late guest has the complete OBS composition and separate self-preview. This also confirms the third guest flow. |
| Run the public stream alongside the return | [View](cropped/17-obs-public-stream.png) | [Full](full/17-obs-public-stream.png) | Stop Streaming button, active output status and source audio meters are visible. This test streamed to a local RTMP recorder. |
| Stop the return independently | [View](cropped/18-return-stopped.png) | [Full](full/18-return-stopped.png) | Return stopped, Start return enabled, Stop return disabled. The separate recorded status check confirms the main stream remained active. |
| Guest reloads and rejoins | [View](cropped/19-guest-rejoined.png) | [Full](full/19-guest-rejoined.png) | OBS composition returns after camera selection and START; measured guest stream ID is unchanged. |
| Correct a missing TURN server | [View](cropped/20-turn-validation.png) | [Full](full/20-turn-validation.png) | Force TURN checked without an entry produces readable inline guidance; no return starts. |
| Settings survive an OBS restart | [View](cropped/21-restored-settings.png) | [Full](full/21-restored-settings.png) | capacity 6, custom signaling, default salt and masked STUN configuration are restored; the return is stopped until manually started. |
| Run with the saved connection settings | [View](cropped/22-custom-network-live.png) | [Full](full/22-custom-network-live.png) | final DLL is running through custom signaling with four viewers out of six; both basic and advanced fields are disabled. |
| Check the narrow dock layout | [View](cropped/23-narrow-dock.png) | [Full](full/23-narrow-dock.png) | labels wrap and buttons/status remain readable. Long IDs scroll within their fields; the normal-width capture is preferable for guides. |

## Reuse notes

- Use the cropped images for instructions; retain originals and the manifest for context.
- The capacity demonstration uses 4 viewers, then 6 after the persistence check. The product default remains 10.
- The initial guest picture is sparse because the OBS scene is populated afterward. Use the full-room images when illustrating the completed experience.
- The normal-width settings, audio-routing and full-room captures are the preferred guide images. Narrow-dock and error captures are supplementary.
- Device permission prompts were automatically granted to synthetic devices. These captures do not document real browser permission prompts or physical device setup.
- Publication and release are still pending; these are source assets for future guides, not proof of a released package.

## Final controls, 20 guests and remote browsers

The [extended manifest](extended/manifest.json) adds 21 full/cropped pairs with visual review notes: final return controls, 21/22 live capacity, 20-guest program, independent stop, persisted settings, and invitation/device/program flow on Firefox, iPhone Safari and Android Chrome. [Measured results](extended/validation-results.json) distinguish the local 20-guest load test from the separate remote relay checks. Permission-transition images do not show native permission dialogs.

The finished guides are [Host setup](../../docs/guest-return.html), [Guest joining](../../docs/guest-return-join.html), and [Pre-show check](../../docs/guest-return-rehearsal.html). Their desktop/mobile full-page and section captures and printable PDFs are in the local `artifacts/guest-return-guides-published-1.1.78/` directory.
