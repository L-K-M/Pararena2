/* port_steal.c — bash steals ("fumbles"): bashing the ball carrier hard
 * enough knocks the ball loose.
 *
 * Design: deterministic, physics-gated. The strip happens when a fresh
 * person-person impact has (a) the non-carrier bashing and (b) a closing
 * speed along the line of impact above kStealMinClosing — a committed
 * full-speed lunge strips, a slow shoulder-rub just shoves. No dice rolls:
 * everything else in this game is Newtonian, and possession shouldn't be
 * the one slot machine in the arena. The ball pops loose *along the hit
 * direction* (past the carrier, away from the basher) so a strip starts a
 * loose-ball scramble rather than handing over possession.
 *
 * Rules riding along:
 *   - a ~1 s grace window after any catch (a pickup isn't instantly punished)
 *   - the loose ball is credited to the BASHER (theBall.modifier /
 *     lastToucher), so a fumble that rolls out of bounds fouls the basher,
 *     not the victim
 *   - the engine's own justHitOpponent cooldown (kLoopsImpactless frames)
 *     already prevents machine-gun re-strips
 *   - disabled entirely in CLASSIC MODE — the untouched 1992 rules have no
 *     steal, and classic mode is the port's gate for gameplay changes
 *
 * Wiring: the verbatim 1v1 collision pass (Dynamics.c HandleCollisions) is
 * compiled as HandleCollisionsGame via a -D on that one file (the same trick
 * Render.c uses for HandlePostGraphics); the HandleCollisions below wraps it
 * with a before/after possession-and-impact check. The four-player engine
 * has its own collision code and calls the shared helpers from there
 * (port_four.c).
 *
 * Tuning: set PARARENA_STEAL_LOG=1 to log every carrier impact with its
 * closing speed and verdict; kStealMinClosing was chosen from those logs
 * over AI demo games (see the PR notes).
 */

#include <math.h>
#include <SDL3/SDL.h>
#include "shim_internal.h"
#include "Globals.h"
#include "Dynamics.h"
#include "SoundUtils.h"
#include "Render.h"
#include "Ball.h"

extern int classicMode;               /* Options toggle (port_shell.c) */

/* the verbatim collision pass (Dynamics.c, renamed by the build) */
void HandleCollisionsGame (void);

/* ---- tuning constants (shared by the 1v1 wrapper and port_four.c) ----
 * Velocity scale for reference: cruising speed tops out around
 * maxBoardForce * kFrictionFraction = 64 * 128 = 8192; a thrown ball gets
 * +1152 (kMaxBoardForceLg * kPersonImpulse / kBallMass). */
#define kStealMinClosing   4200        /* min closing speed along the impact line */
#define kStealGraceTicks   60          /* no strip for ~1 s after any catch */
#define kStealPopSpeed     1250        /* fumble impulse, ~ a thrown ball */
#define kStealPopOffset    2300        /* pop the ball clear of the carrier's disk */

/* Ticks when the current possession began (any mode). The 1v1 wrapper detects
 * the catch by the held-state transition inside the verbatim pass; the 4P
 * engine stamps it in do4Merged. */
long portStealHeldSince;

static int stealLogInited, stealLog;

int PortStealLogging (void)
{
	if (!stealLogInited)
	{
		stealLogInited = 1;
		stealLog = SDL_getenv("PARARENA_STEAL_LOG") != NULL;
	}
	return stealLog;
}

int PortStealEnabled (void)
{
	return !classicMode;
}

int PortStealGraceOver (void)
{
	return Ticks - portStealHeldSince >= kStealGraceTicks;
}

/* closing speed of the basher toward the carrier along the line of centers,
 * from PRE-collision velocities/positions (the momentum exchange scrambles
 * them). Positive = approaching. */
float PortStealClosing (short bshX, short bshZ, short bshVX, short bshVZ,
                        short carX, short carZ, short carVX, short carVZ)
{
	float dx = (float)carX - bshX, dz = (float)carZ - bshZ;
	float d = sqrtf(dx * dx + dz * dz);
	if (d < 1.0f)
		return 0.0f;
	return (((float)bshVX - carVX) * dx + ((float)bshVZ - carVZ) * dz) / d;
}

int PortStealFastEnough (float closing)
{
	return closing >= (float)kStealMinClosing;
}

/* knock the held ball loose: place and launch it along the (pre-impact) hit
 * normal from the carrier's current state. Shared physics for both engines;
 * the callers update their own possession bookkeeping around it. */
void PortStealPopBall (playerType *carrier, float nx, float nz)
{
	PlaySoundSMS(kBallDropSound);

	theBall.xPos = (short)(carrier->xPos + nx * kStealPopOffset);
	theBall.zPos = (short)(carrier->zPos + nz * kStealPopOffset);
	theBall.xVel = (short)(carrier->xVel + nx * kStealPopSpeed);
	theBall.zVel = (short)(carrier->zVel + nz * kStealPopSpeed);
	theBall.justHitWall = 0;
	BallRectFromPosition();
	theBall.mode = kBallRolling;

	carrier->posture = kStanding;
	carrier->justHitBall = kLoopsImpactless;      /* no instant re-touch */
	if ((carrier->persona == kHumanPlayer) || (carrier->persona == kNetHuman))
		carrier->mouseWasLetUp = FALSE;           /* release before re-catching */
}

/* ------------------------------------------------------------- 1v1 wrapper */

void HandleCollisions (void)
{
	/* pre-collision snapshot: possession, impact cooldown, and the incoming
	 * velocities/positions (the exchange overwrites them) */
	short preJHO = thePlayer.justHitOpponent;
	short heldBy = whosGotBall;                   /* kPlayer/kOpponentHasBall */
	char  heldBefore = (theBall.mode == kBallHeld);
	short pVX = thePlayer.xVel, pVZ = thePlayer.zVel;
	short pX  = thePlayer.xPos, pZ  = thePlayer.zPos;
	short oVX = theOpponent.xVel, oVZ = theOpponent.zVel;
	short oX  = theOpponent.xPos, oZ  = theOpponent.zPos;

	HandleCollisionsGame();                       /* the verbatim pass */

	/* a catch happened inside the pass: start the strip grace window */
	if (!heldBefore && theBall.mode == kBallHeld)
		portStealHeldSince = Ticks;

	if (!PortStealEnabled() || netGameInSession)
		return;
	if (!heldBefore || theBall.mode != kBallHeld)
		return;                                   /* nobody was carrying */
	/* fresh person-person impact this pass: the check only fires when
	 * justHitOpponent was 0, and leaves it at kLoopsImpactless-1 */
	if (preJHO != 0 || thePlayer.justHitOpponent == 0)
		return;

	{
		playerType *carrier, *basher;
		float closing, dx, dz, d;

		if (heldBy == kPlayerHasBall)
		{
			carrier = &thePlayer;
			basher = &theOpponent;
			closing = PortStealClosing(oX, oZ, oVX, oVZ, pX, pZ, pVX, pVZ);
			dx = (float)pX - oX;  dz = (float)pZ - oZ;
		}
		else if (heldBy == kOpponentHasBall)
		{
			carrier = &theOpponent;
			basher = &thePlayer;
			closing = PortStealClosing(pX, pZ, pVX, pVZ, oX, oZ, oVX, oVZ);
			dx = (float)oX - pX;  dz = (float)oZ - pZ;
		}
		else
			return;

		if (PortStealLogging())
			ShimLog("steal(1v1): closing=%.0f bash=%d grace=%d -> %s",
			        (double)closing, basher->bashApplied, !PortStealGraceOver(),
			        (basher->bashApplied && PortStealGraceOver() &&
			         PortStealFastEnough(closing)) ? "FUMBLE" : "shove");

		if (!basher->bashApplied || !PortStealGraceOver() ||
		    !PortStealFastEnough(closing))
			return;

		d = sqrtf(dx * dx + dz * dz);
		if (d < 1.0f)
			return;
		PortStealPopBall(carrier, dx / d, dz / d);

		whosGotBall = kBallRollsFreely;
		/* the loose ball belongs to the BASHER: if it rolls out of bounds,
		 * DoAFoul() reads theBall.modifier and fouls them, not the victim */
		theBall.modifier = (basher->selector == kPlayerSelector)
		                   ? kPlayerLastHeld : kOpponentLastHeld;

		/* keep Ms. Teak's release tracking coherent (as DoPersonBallParted does) */
		if (thePlayer.persona == kMissTeak)
			oldDistSquared = ((long)thePlayer.xPos * thePlayer.xPos) +
			                 ((long)thePlayer.zPos * thePlayer.zPos);
		else if (theOpponent.persona == kMissTeak)
			oldDistSquared = ((long)theOpponent.xPos * theOpponent.xPos) +
			                 ((long)theOpponent.zPos * theOpponent.zPos);

		UpdateArrows();
	}
}
