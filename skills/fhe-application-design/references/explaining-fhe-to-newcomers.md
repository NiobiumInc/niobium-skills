# Explaining FHE to a newcomer

Read this when the user is new to Fully Homomorphic Encryption (FHE), or when a
user who said they were experienced turns out to want plainer explanations. It is
the teaching companion to the design stages: the stages tell you *what* to build,
this tells you *how to talk about it* so the person you are working with can follow
along and step in when they want to.

The goal is not to hide the real engineering. It is to make sure the user always
knows what is happening, why, and what it costs them, without needing to read the
skill or learn cryptography first. The precise numbers still go in the code and the
report tables; this is about the conversation and the plain-language parts of the
docs.

**Who to assume you are talking to, and how to pitch it.** Picture a sharp product
person or engineer: fluent in code and tradeoffs, with no interest in the underlying
math. They do not care what a polynomial is or how high a degree is. They care what
a setting *gives them* and what it *costs* them: more accurate but slower, cheaper
but rejects a few more cases, and so on. So explain every term and every choice in
"why it matters" terms, as a functional tradeoff. Only reach for a mechanism when it
actually helps them make the call in front of them; otherwise leave it in the code
and the tables. This is a product exercise, not a lecture.

## Two habits that matter most

1. **Say what a thing buys the user and what it costs, not just its name.** A
   parameter or a term on its own ("Chebyshev degree 59") tells a newcomer nothing.
   The same fact framed as a decision they can make ("a higher-accuracy setting that
   adds about half an hour of compute for a fraction of a percent more agreement")
   lets them actually weigh in.

2. **Name the parties as the people they are.** In this application the **client**
   is the user's side: it holds the real data and the secret key, and it is the only
   side that ever sees a plaintext value or a result. The **server** is the other
   party, the one that runs the computation without being allowed to see the data
   (name it concretely for the workload: the vendor, the cloud provider, the
   analytics company). Say "your side" and "the vendor's side" before you say
   "client" and "server," and keep doing it until the user is clearly comfortable.

## Naming the parties

- **Client = you (the user).** Holds the data, generates the keys, keeps the secret
  key, encrypts the inputs, and decrypts the result. Nothing sensitive leaves this
  side in the clear.
- **Server = the party computing for you.** Receives encrypted inputs, runs the
  math on them while they stay encrypted, and sends an encrypted answer back. It
  never holds the secret key, so it never sees the data or the answer.
- **Secret key = the only thing that can decrypt.** It stays on your side. If it is
  never sent to the server, the server cannot read anything, which is the whole
  point.

A one-sentence version to open with: "Your side encrypts the data and keeps the
only key; their side does the computation on the encrypted data and never sees it."

## Explaining a result

When you report that the encrypted run matched the plaintext one, a newcomer needs
to know what that comparison *is*, not just a number:

- **The reference (the answer key).** The ordinary, un-encrypted version of the
  computation. It is the ground truth: what the right answers are.
- **The twin.** A stand-in for the encrypted computation that runs in the clear so
  it is fast to check. If the twin matches the answer key, the design is right
  before any encryption is involved.
- **The encrypted run vs. the twin.** This last comparison is the one that isolates
  encryption itself. When it matches, the plain-language takeaway is simply:
  "encrypting the computation did not change the answers."

So "100% agreement, 0 flips" means "the encrypted version gave the same decisions as
the ordinary version on every case we tried." That is the sentence to say. The raw
error figures belong in the report table underneath.

Explaining *why any small difference exists* (when it does): to run under
encryption, some steps of the computation are approximated rather than done exactly,
and encryption itself adds a tiny bit of fuzz. Both are measured and kept within a
budget set in advance, so a handful of close-call cases coming out differently is
expected and does not mean anything is broken.

## Framing a tradeoff as a decision

Most design choices are a tradeoff between quality and cost. A newcomer can decide
these easily *if* you give them both sides in terms they feel. The pattern:

> Option A gives *this quality* at *this cost*. Option B gives *this better quality*
> at *this higher cost*. Here is which I recommend and why.

- If two options are the **same speed and cost**, the higher-quality one is simply
  better; take it and say so. "Same run time either way, so we take the more
  accurate one."
- If the better option **costs real time or money**, name the cost and let the size
  of the improvement speak. "100% agreement instead of 99.97% sounds nicer, but it
  is fifteen cases in fifty thousand, and buying it costs about thirty extra minutes
  per run. I would take the cheaper one unless those fifteen matter to you."
- Always give a **recommendation**, not just the menu. The user is trusting you to
  have an opinion; the choice is theirs, but the default should be clear.

## What each term means for the user (functional, not academic)

Use these when a term is unavoidable. Lead with what it gives the user or costs
them. If a term is just a knob, describe the knob's effect and skip what it is.

- **Accuracy / agreement.** How often the answers are right (against the answer key)
  or the same (between two versions). This is the number the user actually cares
  about; lead with it.
- **Base rate.** How common the thing being predicted is to begin with (for example,
  20% of ad views get clicked). It is the yardstick that says whether a score is
  good; "80% accurate" means little without it.
- **Flip.** One case where the answer changed between two versions. "Fifteen flips
  out of fifty thousand" is fifteen cases that came out differently.
- **Close call (what the skill calls a "boundary tie").** A case where two possible
  answers scored almost equally, so a tiny change tips it. When flips happen, they
  are almost always these close calls, not real mistakes. Say "close calls."
- **Recall (for a specific class).** Of the cases that really were, say, "rework,"
  how many the model caught. It matters when missing the important class is costly,
  so preserving it is often the real pass condition.
- **The reference model.** The ordinary, un-encrypted program that defines the right
  answers. Call it "the ordinary version" or "the answer key."
- **The twin.** The fast un-encrypted stand-in used to check the design quickly.
  Call it "the stand-in we check against."
- **Degree.** An accuracy dial. Higher is more accurate but slower and more
  expensive to run. That is the whole story the user needs; the number itself does
  not matter to them. Present it as "more accurate vs. faster."
- **Depth.** The main cost driver. More of it means bigger data and slower runs, so
  the design keeps it as low as it can. Present it as "how heavy the computation is."
- **Scaling / precision.** How much fine detail is kept. More is more precise but
  more expensive. Another accuracy-vs-cost dial.
- **Ring dimension.** A security size setting, fixed here at the safe default. Not
  worth discussing beyond "it is set for security."
- **Decode-safe.** Whether the final answer still comes out readable at the end.
  "Decode-safe" just means "yes, the answer comes out clean." A design has to be this
  before it is worth running.
- **Noise budget.** Encryption adds a little fuzz that grows as the computation runs,
  and there is a fixed amount you can spend. Present it as "how much computation we
  can fit before the answer gets fuzzy."
- **Bootstrapping.** An expensive refresh that lets a longer computation keep going.
  The design avoids it when it can because it is slow; mention it only if the
  workload forces it, framed as "a costly step this design does / does not need."
- **Niobium Fog.** The platform these applications are built to run on. The default
  run sends the computation to it; the local modes are for checking first.

## Explaining why an artifact exists

When you generate a file or a step, say in a sentence why it is there, so the user
is not watching output appear without meaning. A few common ones:

- **The plaintext reference / answer key.** "First we build the ordinary version, so
  we have something correct to check every later version against."
- **compare_results (or the run_test comparison).** "This checks the encrypted
  answers against the ordinary ones and reports whether they match, so we can prove
  encryption did not change the outcome."
- **feature_bounds (the input bounds file).** "This is the safe range for each input;
  your side checks against it before encrypting, because an out-of-range value would
  break the encrypted answer."
- **keygen / the secret key on your side only.** "This makes the keys. The secret one
  stays with you; the server only ever gets the keys that let it compute, not read."
- **The two-process demo.** "This runs your side and the vendor's side as genuinely
  separate programs, to show the vendor really never sees your data or your key."

## Narrating the stages

At the start, give the user the map in a few plain sentences: we work out who needs
to see what, check the problem is a fit for encryption, build and check the ordinary
version, tune it to run under encryption, prove it still gives the same answers, then
build and run the encrypted version. As you enter each stage, say which one it is and
why it matters in one line. This keeps the user oriented enough to redirect you,
without turning every step into a permission request.
