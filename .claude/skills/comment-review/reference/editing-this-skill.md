# Editing this skill

State a rule with `must`, `must not`, `should`, or `may` — the RFC 2119 sense — rather
than a sentence that implies the same requirement without naming it. "What a comment may
be about depends on where it sits" says `may` without the word; write the word. A reader
checking a rule against a comment needs the requirement in one place, not assembled from
a description of how things generally work out.

This does not reach the comments this skill reviews. Pass 4's modal rule governs those,
and `should` is a hedge there for a different reason: code either does a thing or it does
not, and a review has no finding to recommend rather than state.
