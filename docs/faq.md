# FAQ

## Is this legal?

The Helgatchi is **passive**: it listens to broadcasts that devices send in
the clear to everyone nearby. It does not transmit while scanning, does not
deauth, does not jam, does not connect to anything. Laws vary — you're
responsible for knowing yours.

## Does it attack anything?

No. It's a receiver with opinions.

## What's the range?

Whatever the target broadcasts at — typically tens of meters for BLE, more for
Wi-Fi APs, heavily dependent on the antenna and environment.

<!-- TODO: real-world numbers with the stock antenna -->

## How long does the battery last?

<!-- TODO: numbers per battery size at the default duty cycle, and in foxhunt
     mode -->

## How is this different from a Flipper Zero / Pwnagotchi?

Single purpose: it detects and identifies nearby BLE/Wi-Fi devices against a
curated, user-extensible rules library, and it does it passively on a duty
cycle so it can live in your pocket all day. It's a tripwire, not a multitool.

## Can it detect AirTags?

<!-- TODO: honest answer re: Apple MAC randomization / Find My rotation, what
     the tracker rulesets can and can't catch -->

## Can I add my own detections?

Yes — that's the point. See [Detection rules](rules.md).

## Why "Helgatchi"?

<!-- TODO: the Helga story -->

## Who makes this?

<!-- TODO: DC801 / 801 Labs blurb + links -->
