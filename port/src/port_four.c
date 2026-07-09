/* port_four.c — four-player modes for the port: 2v2, free-for-all on the two
 * classic goals with rotating goal ownership, and free-for-all on four goals
 * (the force table's mirror symmetry provides the two southern goals; their
 * markers are painted the same way UpdateGoalPicts paints the northern ones).
 *
 * Nothing in Sources/ changes. The engine reuses the verbatim primitives that
 * already take playerType* (MovePerson-style movement is reimplemented here
 * only because the mode-C rules change the out-of-bounds gate), and drives the
 * verbatim persona AI through a "pair view": each AI seat sees itself as
 * theOpponent and its chosen target as thePlayer for the duration of its
 * decision call, which is exactly the world the 1992 code expects.
 */

#include <math.h>
#include <SDL3/SDL.h>
#include "shim_internal.h"
#include "Globals.h"
#include "UnivUtilities.h"
#include "CommonPerson.h"
#include "Dynamics.h"
#include "Ball.h"
#include "Human.h"
#include "Computer.h"
#include "Render.h"
#include "RenderQD.h"
#include "PlayUtils.h"
#include "PlayCore.h"
#include "SoundUtils.h"
#include "MainWindow.h"
#include "InitGameStructs.h"
#include "DissBits.h"       /* DissolveWorkToMain (splash-to-arena reveal) */

/* shim_input.c */
int  ShimPadRead (int idx, float *x, float *y, int *btn, int *brake, int *bash);
void PortInputSetPlayMode (int playing);
/* port_steal.c — bash steals (fumbles), shared with the 1v1 wrapper */
extern long portStealHeldSince;
int   PortStealEnabled (void);
int   PortStealGraceOver (void);
int   PortStealFastEnough (float closing);
int   PortStealLogging (void);
float PortStealClosing (short bshX, short bshZ, short bshVX, short bshVZ,
                        short carX, short carZ, short carVX, short carVZ);
void  PortStealPopBall (playerType *carrier, float nx, float nz);
/* port_shell.c */
void DoOpeningAnnouncer (void);
void DrawControlsCard (const char *title);   /* menu controls card */
void DrawPauseScreen (void);                 /* full-screen pause art */
void PortDrawMobileControls (void);          /* on-screen touch controls overlay */
extern int classicMode;                      /* Options toggle: hide HUD enhancements */
const char   *PortPlayerName (int seat);     /* pre-match setup: name for the plate */
unsigned char PortPlayerColor (int seat);    /* pre-match setup: badge colour */

/* ---------------------------------------------------------------- state */

enum { FOUR_OFF = 0, FOUR_2V2 = 1, FOUR_FFA2 = 2, FOUR_FFA4 = 3 };

static playerType extraA, extraB;      /* seats 2 and 3 */
/* seat order: 0 = left/thePlayer, 1 = right/theOpponent, 2 = left partner,
 * 3 = right partner. Teams (2v2): {0,2} vs {1,3}. */
static playerType *ent[4] = { &thePlayer, &theOpponent, &extraA, &extraB };

static int fourMode;                   /* FOUR_* while a 4P game runs */
static int seatPersona[4];             /* kHumanPlayer or kSimpleGeorge..kMissTeak */
static int seatSlot[4];                /* input slot for human seats: 0=kb/mouse, 1..3=pads */
static int seatBank[4];                /* sprite bank 0/1 */
static int score4[4], fouls4[4];
static int ballOwner = -1;             /* seat holding the ball, -1 = none */
static int lastToucher = -1;
static int goalOwner[4];               /* seat owning goal g (B: g=0,1; C: g=0..3) */
static long nextSwitchTick;            /* mode B rotation */
static int nextSwitchGoal;
static int aiTarget[4];
static int fourDone, fourWinner;
static Boolean savedCanReplay;

/* per-seat identity colors (Apple palette): green, cyan, orange, magenta */
static const unsigned char seatColor[4] = { 8, 7, 2, 4 };
/* FFA sprite recolor: seat2 tints bank0 green->orange, seat3 bank1 cyan/blue->magenta/purple */
static const unsigned char remapSeat2[16] = { 0,1,2,3,4,5,6,7, 2,10, 10,11,12,13,14,15 };
static const unsigned char remapSeat3[16] = { 0,1,2,3,4,5, 5,4, 8,9, 10,11,12,13,14,15 };

#define TEAM_OF(s)   ((s) & 1)         /* 0 = left team, 1 = right team */
#define FFA_WIN      7

/* ---------------------------------------------------------------- physics */

static short *fourForceTable (void)
{
	switch (isLeague)
	{
		case kLittleLeague:  return littleForceTable;
		case kJuniorVarsity: return juniorForceTable;
		case kVarsity:       return varsityForceTable;
		case kMinorLeague:   return minorForceTable;
		default:             return proForceTable;
	}
}

/* force at (x,z); returns the sentinel (kOutOBounds/kBackBoard/kGoalPath) or 0
 * with the signed force components */
static short forceAt (short xPos, short zPos, short *fxOut, short *fzOut)
{
	short *table = fourForceTable();
	short ix = xPos, iz = zPos, sx = 1, sz = 1;
	if (ix < 0) { ix = -ix; sx = -1; }
	if (iz < 0) { iz = -iz; sz = -1; }
	ix /= 512;
	iz /= 512;
	short fx = *(table + ix * 82 + iz * 2 + kXComponent);
	if (fx == kOutOBounds || fx == kBackBoard || fx == kGoalPath)
		return fx;
	*fxOut = (short)(fx * sx);
	*fzOut = (short)(*(table + ix * 82 + iz * 2 + kZComponent) * sz);
	return 0;
}

/* in modes A/B the southern halves of the backboard/goal cells fall out of
 * bounds exactly like the original; mode C un-gates them (four corner goals) */
static int wallIsReal (short zPos)
{
	return fourMode == FOUR_FFA4 || zPos >= 0;
}

static void move4Person (playerType *who)
{
	short fx = 0, fz = 0;
	short sentinel = forceAt(who->xPos, who->zPos, &fx, &fz);

	switch (sentinel)
	{
		case kOutOBounds:
			who->flag = kIsOutOfBounds;
			break;
		case kGoalPath:
		case kBackBoard:
			if (!wallIsReal(who->zPos))
			{
				who->flag = kIsOutOfBounds;
			}
			else
			{
				HandlePersonWallCollision(who);      /* verbatim rebound */
				who->xPos += (who->xVel / kVelocitySensitive);
				who->zPos += (who->zVel / kVelocitySensitive);
			}
			break;
		default:
			who->flag = kIsNormal;
			who->xVel -= fx;
			who->zVel -= fz;
			if (who->justHitWall == 0)
			{
				who->xVel -= who->xVel / kFrictionFraction;
				who->zVel -= who->zVel / kFrictionFraction;
			}
			who->xPos += (who->xVel / kVelocitySensitive);
			who->zPos += (who->zVel / kVelocitySensitive);
			break;
	}
	PersonRectFromPosition(who);
}

/* CheckUpOnBall equivalent that respects the mode-C gate */
static void check4UpOnBall (void)
{
	short fx, fz;
	if (forceAt(theBall.xPos, theBall.zPos, &fx, &fz) == kBackBoard &&
	    wallIsReal(theBall.zPos))
	{
		if ((theBall.zPos >= 0 && theBall.zVel > 0) ||
		    (theBall.zPos < 0 && theBall.zVel < 0))
			theBall.zVel *= -1;
		if (theBall.xPos < 0)
		{
			if (theBall.xVel < 0)
				theBall.xVel *= -1;
		}
		else
		{
			if (theBall.xVel > 0)
				theBall.xVel *= -1;
		}
	}
}

static void ball4WallCollision (void)
{
	short tempVel;
	if (theBall.justHitWall != 0)
	{
		theBall.flag = kIsNormal;
		return;
	}
	PlaySoundSMS(kRicochetSound);
	theBall.flag = kIsRebounding;
	theBall.justHitWall = kLoopsImpactless;
	for (int i = 0; i < 4; i++)
		ent[i]->justHitBall = 0;
	theBall.xPos -= (theBall.xVel / kVelocitySensitive);
	theBall.zPos -= (theBall.zVel / kVelocitySensitive);
	if (theBall.xPos < 0)
	{
		tempVel = theBall.xVel;
		theBall.xVel = theBall.zVel;
		theBall.zVel = tempVel;
	}
	else
	{
		tempVel = theBall.xVel;
		theBall.xVel = -theBall.zVel;
		theBall.zVel = -tempVel;
	}
	check4UpOnBall();
}

/* which of the four goals is the ball in? modes A/B: 0=left, 1=right;
 * mode C: 0=NW 1=NE 2=SW 3=SE */
static int goalAtBall (void)
{
	if (fourMode != FOUR_FFA4)
		return theBall.xPos < 0 ? 0 : 1;
	if (theBall.zPos >= 0)
		return theBall.xPos < 0 ? 0 : 1;
	return theBall.xPos < 0 ? 2 : 3;
}

static void move4Ball (void)
{
	#define kOffTheDishLg4 473497600L
	short fx = 0, fz = 0;
	short sentinel = forceAt(theBall.xPos, theBall.zPos, &fx, &fz);

	switch (sentinel)
	{
		case kOutOBounds:
			theBall.flag = kIsOutOfBounds;
			break;
		case kBackBoard:
			if (!wallIsReal(theBall.zPos))
				theBall.flag = kIsOutOfBounds;
			else
			{
				ball4WallCollision();
				theBall.xPos += (theBall.xVel / kVelocitySensitive);
				theBall.zPos += (theBall.zVel / kVelocitySensitive);
			}
			break;
		case kGoalPath:
			if (!wallIsReal(theBall.zPos))
				theBall.flag = kIsOutOfBounds;
			else
				theBall.flag = kIsInGoal;
			break;
		default:
			theBall.flag = kIsNormal;
			theBall.xVel -= fx;
			theBall.zVel -= fz;
			if (theBall.justHitWall == 0)
			{
				theBall.xVel -= theBall.xVel / kFrictionFraction;
				theBall.zVel -= theBall.zVel / kFrictionFraction;
			}
			theBall.xPos += (theBall.xVel / kVelocitySensitive);
			theBall.zPos += (theBall.zVel / kVelocitySensitive);
			break;
	}

	long d2 = ((long)theBall.xPos * theBall.xPos) + ((long)theBall.zPos * theBall.zPos);
	theBall.dontDraw = (d2 > kOffTheDishLg4);
	BallRectFromPosition();
}

/* ---------------------------------------------------------------- possession */

static void syncLegacyBallState (void)
{
	/* keep the 2-player globals coherent for the verbatim HUD (arrows, bars)
	 * in 2v2; in FFA they stay neutral */
	if (fourMode == FOUR_2V2)
	{
		if (ballOwner < 0)
		{
			whosGotBall = (theBall.mode == kBallRolling) ? kBallRollsFreely : kBallIsNotHere;
			/* only stamp last-held once the ball is actually in play — writing it
			 * during the kBallFiring launch window clobbers ResetBall's ~2s
			 * countdown (which lives in theBall.modifier), re-firing in a few frames */
			if (lastToucher >= 0 && theBall.mode == kBallRolling)
				theBall.modifier = TEAM_OF(lastToucher) == 0 ? kPlayerLastHeld : kOpponentLastHeld;
		}
		else if (TEAM_OF(ballOwner) == 0)
		{
			whosGotBall = kPlayerHasBall;
			theBall.modifier = kPlayerHolding;
		}
		else
		{
			whosGotBall = kOpponentHasBall;
			theBall.modifier = kOpponentHolding;
		}
	}
}

static void do4Merged (int seat)
{
	playerType *who = ent[seat];
	long xM = (who->xVel * kPersonMass) + (theBall.xVel * kBallMass);
	long zM = (who->zVel * kPersonMass) + (theBall.zVel * kBallMass);

	PlaySoundSMS(kBallPickUpSound);
	who->xVel = (short)(xM / kPersonBallMass);
	who->zVel = (short)(zM / kPersonBallMass);

	if (who->persona == kHumanPlayer)
		who->mouseWasLetUp = FALSE;
	else if (who->persona == kMisterEaze)
		who->strategy = (short)RandomCoin();
	else if (who->persona == kMissTeak)
		who->strategy = (RandomInt(100) < who->teaksThresh) ? kRunDiagonal : kRunCircle;

	theBall.eraseTheBall = TRUE;
	drawThisFrame = TRUE;
	who->posture = kCarrying;
	theBall.mode = kBallHeld;
	ballOwner = seat;
	lastToucher = seat;
	portStealHeldSince = Ticks;           /* bash-steal grace window starts */
	for (int i = 0; i < 4; i++)
		ent[i]->loopsBallHeld = 0;
	who->loopsBallHeld = kLoopLimitOnHeldBall;
	syncLegacyBallState();
	if (fourMode == FOUR_2V2)
	{
		UpdateBallTimers(who);
		UpdateArrows();
	}
}

static void do4Parted (int seat)
{
	#define kOffsetMultiplier4 2
	playerType *who = ent[seat];
	short dPX = (short)(boardForceTable[who->direction][kXComponent] * kPersonImpulse);
	short dPZ = (short)(boardForceTable[who->direction][kZComponent] * kPersonImpulse);

	PlaySoundSMS(kBallDropSound);
	theBall.xVel = who->xVel + (dPX / kBallMass);
	theBall.zVel = who->zVel + (dPZ / kBallMass);
	theBall.xPos = who->xPos + (dPX * kOffsetMultiplier4);
	theBall.zPos = who->zPos + (dPZ * kOffsetMultiplier4);
	BallRectFromPosition();

	who->xVel -= dPX / kPersonMass;
	who->zVel -= dPZ / kPersonMass;
	who->posture = kCrouching;
	theBall.mode = kBallRolling;
	ballOwner = -1;
	lastToucher = seat;
	syncLegacyBallState();

	for (int i = 0; i < 4; i++)
		if (ent[i]->persona == kMissTeak)
			oldDistSquared = ((long)ent[i]->xPos * ent[i]->xPos) +
			                 ((long)ent[i]->zPos * ent[i]->zPos);
	if (who->persona == kHumanPlayer)
		who->mouseWasLetUp = FALSE;
	if (fourMode == FOUR_2V2)
		UpdateArrows();
}

static void check4PersonBall (int seat)
{
	#define kSqrImpactPB4 1478656L
	playerType *who = ent[seat];
	long dx, d2;

	if ((who->mode != kInArena) || (theBall.mode != kBallRolling) || (who->justHitBall != 0))
		return;
	dx = ((long)theBall.xPos - who->xPos) * ((long)theBall.xPos - who->xPos);
	if (dx >= kSqrImpactPB4)
		return;
	d2 = dx + ((long)theBall.zPos - who->zPos) * ((long)theBall.zPos - who->zPos);
	if (d2 >= kSqrImpactPB4)
		return;

	if ((who->posture == kCrouching) && (who->mouseWasLetUp))
	{
		do4Merged(seat);
	}
	else
	{
		PlaySoundSMS(kClashSound);
		DoPersonBallCollided(who);          /* verbatim bounce math */
		who->justHitBall = kLoopsImpactless;
		check4UpOnBall();
		lastToucher = seat;
		syncLegacyBallState();
	}
}

/* generalized DoPersonPersonCollided + separation (verbatim math, two pointers) */
static void do4PersonPerson (playerType *a, playerType *b)
{
	#define kLimitScale4 1024L
	short o2x, o2z;
	long dx, dz, d2, s1, s2;
	long n1x = 0, n1z = 0, n2x = 0, n2z = 0;

	PlaySoundSMS(kClashSound);
	a->justHitOpponent = kLoopsImpactless;
	b->justHitOpponent = kLoopsImpactless;
	a->justHitWall = 0;
	b->justHitWall = 0;

	a->xVel -= a->xVel / kEnergyAbsorbed;
	a->zVel -= a->zVel / kEnergyAbsorbed;
	b->xVel -= b->xVel / kEnergyAbsorbed;
	b->zVel -= b->zVel / kEnergyAbsorbed;

	o2x = b->xVel;
	o2z = b->zVel;
	a->xVel -= o2x;
	a->zVel -= o2z;
	b->xVel = 0;
	b->zVel = 0;

	dx = (long)b->xPos - a->xPos;
	dz = (long)b->zPos - a->zPos;
	d2 = ((dx * dx) + (dz * dz)) / kLimitScale4;
	s1 = (((long)-a->xVel * dz) + ((long)a->zVel * dx)) / kLimitScale4;
	s2 = (((long)a->xVel * dx) + ((long)a->zVel * dz)) / kLimitScale4;
	if (d2 != 0)
	{
		n1x = -s1 * dz / d2;
		n1z = s1 * dx / d2;
		n2x = s2 * dx / d2;
		n2z = s2 * dz / d2;
	}
	a->xVel = (short)n1x + o2x;
	a->zVel = (short)n1z + o2z;
	b->xVel = (short)n2x + o2x;
	b->zVel = (short)n2z + o2z;
}

static void check4PersonPerson (int ia, int ib)
{
	#define kSqrImpactPP4 2560000L
	playerType *a = ent[ia], *b = ent[ib];
	long dx, d2;
	short aX, aZ, aVX, aVZ, bX, bZ, bVX, bVZ;

	if ((a->mode != kInArena) || (b->mode != kInArena) || (a->justHitOpponent != 0))
		return;
	dx = ((long)b->xPos - a->xPos) * ((long)b->xPos - a->xPos);
	if (dx >= kSqrImpactPP4)
		return;
	d2 = dx + ((long)b->zPos - a->zPos) * ((long)b->zPos - a->zPos);
	if (d2 >= kSqrImpactPP4)
		return;

	/* pre-impact snapshot for the bash-steal check (the momentum exchange
	 * overwrites the incoming velocities) */
	aX = a->xPos; aZ = a->zPos; aVX = a->xVel; aVZ = a->zVel;
	bX = b->xPos; bZ = b->zPos; bVX = b->xVel; bVZ = b->zVel;

	do4PersonPerson(a, b);
	{
		/* the original separation loop relies on the impact leaving some
		 * relative velocity; with four converging entities the zero-velocity
		 * case actually happens, so nudge them apart directly if it stalls */
		int guard = 0;
		do
		{
			a->xPos += (a->xVel / kVelocitySensitive);
			a->zPos += (a->zVel / kVelocitySensitive);
			b->xPos += (b->xVel / kVelocitySensitive);
			b->zPos += (b->zVel / kVelocitySensitive);
			if (++guard > 32)
			{
				short px = (short)((b->xPos >= a->xPos) ? 64 : -64);
				short pz = (short)((b->zPos >= a->zPos) ? 64 : -64);
				a->xPos -= px;
				b->xPos += px;
				a->zPos -= pz;
				b->zPos += pz;
			}
			dx = ((long)b->xPos - a->xPos) * ((long)b->xPos - a->xPos);
			d2 = dx + ((long)b->zPos - a->zPos) * ((long)b->zPos - a->zPos);
		} while (d2 < kSqrImpactPP4 && guard < 200);
	}

	/* bash steal (port_steal.c): a fresh impact on the carrier at closing
	 * speed knocks the ball loose. Skipped between 2v2 teammates — stripping
	 * your partner is pure grief. The fumbled ball is credited to the basher
	 * (lastToucher), so rolling it out of bounds fouls them, not the victim. */
	if (PortStealEnabled() && ballOwner >= 0 &&
	    (ballOwner == ia || ballOwner == ib))
	{
		int carSeat = ballOwner;
		int bshSeat = (carSeat == ia) ? ib : ia;
		playerType *carrier = ent[carSeat];
		playerType *basher = ent[bshSeat];
		float closing, ndx, ndz, nd;

		if (fourMode == FOUR_2V2 && TEAM_OF(carSeat) == TEAM_OF(bshSeat))
			return;

		if (carSeat == ia)
			closing = PortStealClosing(bX, bZ, bVX, bVZ, aX, aZ, aVX, aVZ);
		else
			closing = PortStealClosing(aX, aZ, aVX, aVZ, bX, bZ, bVX, bVZ);

		if (PortStealLogging())
			ShimLog("steal(4P): P%d->P%d closing=%.0f bash=%d grace=%d -> %s",
			        bshSeat + 1, carSeat + 1, (double)closing,
			        basher->bashApplied, !PortStealGraceOver(),
			        (basher->bashApplied && PortStealGraceOver() &&
			         PortStealFastEnough(closing)) ? "FUMBLE" : "shove");

		if (!basher->bashApplied || !PortStealGraceOver() ||
		    !PortStealFastEnough(closing))
			return;

		/* pre-impact normal from basher through carrier */
		ndx = (float)((carSeat == ia) ? aX - bX : bX - aX);
		ndz = (float)((carSeat == ia) ? aZ - bZ : bZ - aZ);
		nd = sqrtf(ndx * ndx + ndz * ndz);
		if (nd < 1.0f)
			return;
		PortStealPopBall(carrier, ndx / nd, ndz / nd);

		ballOwner = -1;
		lastToucher = bshSeat;
		syncLegacyBallState();
		if (fourMode == FOUR_2V2)
			UpdateArrows();
	}
}

static void handle4Collisions (void)
{
	for (int i = 0; i < 4; i++)
		check4PersonBall(i);
	for (int i = 0; i < 4; i++)
		for (int j = i + 1; j < 4; j++)
			check4PersonPerson(i, j);

	for (int i = 0; i < 4; i++)
	{
		playerType *p = ent[i];
		if (p->justHitWall > 0)
		{
			p->justHitWall--;
			if (p->justHitWall == kFrameToDampen)
			{
				p->xVel -= p->xVel / kPersonDampening;
				p->zVel -= p->zVel / kPersonDampening;
			}
		}
		if (p->justHitBall > 0)
			p->justHitBall--;
		if (p->justHitOpponent > 0)
			p->justHitOpponent--;
	}
	if (theBall.justHitWall > 0)
	{
		theBall.justHitWall--;
		if (theBall.justHitWall == kFrameToDampen)
		{
			theBall.xVel /= kBallDampening;
			theBall.zVel /= kBallDampening;
		}
	}
}

/* ---------------------------------------------------------------- input */

static void apply4HumanForces (int seat)
{
	playerType *who = ent[seat];
	int slot = seatSlot[seat];
	short deflectHalf = (short)((mouseFrame.right - mouseFrame.left) / 2);
	Boolean btn = FALSE;
	int brake = 0, bash = 0;

	if (slot == 0)
	{
		/* keyboard + mouse (the classic path, read directly so a gamepad
		 * driving another seat cannot bleed into this one) */
		Point m;
		GetMouse(&m);
		LocalToGlobal(&m);
		if (ForcePointInRect(&m, &mouseFrame))
			SetMouse(m);
		who->hMouse = (short)((m.h - screenHCenter) / kPlayerInputSensitive);
		who->vMouse = (short)((screenVCenter - m.v) / kPlayerInputSensitive);
		const bool *ks = SDL_GetKeyboardState(NULL);
		btn = (Boolean)(((SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0) ||
		                 ks[SDL_SCANCODE_X]);
		brake = ks[SDL_SCANCODE_SPACE];
		bash = ks[SDL_SCANCODE_B] || ks[SDL_SCANCODE_N] || ks[SDL_SCANCODE_M];
		/* the merged shim state carries the touch controls (swipe zones and the
		 * on-screen catch/brake/bash buttons); in split-input mode it excludes
		 * the pads, which belong to the other seats — so P1 keeps touch input
		 * in four-player games too */
		btn = (Boolean)(btn || shimInput.buttonDown);
		brake = brake || shimInput.brakeDown;
		bash = bash || shimInput.bashDown;
		/* P1 may also use the first gamepad: its stick overrides the mouse while
		 * deflected, and its buttons fold in alongside the keyboard/mouse ones */
		{
			float px = 0, py = 0;
			int pb = 0, pbk = 0, pbs = 0;
			if (ShimPadRead(0, &px, &py, &pb, &pbk, &pbs))
			{
				if (px != 0 || py != 0)
				{
					who->hMouse = (short)((int)(px * deflectHalf) / kPlayerInputSensitive);
					who->vMouse = (short)((int)(-py * deflectHalf) / kPlayerInputSensitive);
				}
				btn = (Boolean)(btn || pb);
				brake = brake || pbk;
				bash = bash || pbs;
			}
		}
	}
	else
	{
		float px = 0, py = 0;
		int b = 0, bk = 0, bs = 0;
		ShimPadRead(slot - 1, &px, &py, &b, &bk, &bs);
		who->hMouse = (short)((int)(px * deflectHalf) / kPlayerInputSensitive);
		who->vMouse = (short)((int)(-py * deflectHalf) / kPlayerInputSensitive);
		btn = (Boolean)b;
		brake = bk;
		bash = bs;
	}
	who->buttonIs = btn;
	DetermineHumanFacing(who);

	short bx = who->hMouse, bz = who->vMouse;
	if (bx > maxBoardForce) bx = maxBoardForce;
	else if (bx < -maxBoardForce) bx = -maxBoardForce;
	if (bz > maxBoardForce) bz = maxBoardForce;
	else if (bz < -maxBoardForce) bz = -maxBoardForce;

	who->bashApplied = FALSE;
	if ((ballOwner != seat) && bash && !brake)
	{
		who->bashApplied = TRUE;
		bx = (short)(boardForceTable[who->direction][kXComponent] * 3);
		bz = (short)(boardForceTable[who->direction][kZComponent] * 3);
	}
	if (btn && !who->bashApplied)
	{
		bx += bx / kAddForceFract;
		bz += bz / kAddForceFract;
	}
	if (who->justHitWall == 0)
	{
		who->xVel += bx;
		who->zVel += bz;
	}

	if (brake && !who->brakeApplied)
		PlaySoundSMS(kBrakeSound);
	who->brakeApplied = (char)brake;
	if (brake)
	{
		who->xVel -= who->xVel / 10;
		who->zVel -= who->zVel / 10;
	}

	if (btn)
	{
		if (ballOwner == seat && who->mouseWasLetUp)
			do4Parted(seat);
		who->posture = kCrouching;
	}
	else
	{
		who->mouseWasLetUp = TRUE;
		who->posture = (ballOwner == seat) ? kCarrying : kStanding;
	}
}

/* ---------------------------------------------------------------- AI */

static int pickTarget (int seat)
{
	if (ballOwner >= 0 && ballOwner != seat &&
	    (fourMode != FOUR_2V2 || TEAM_OF(ballOwner) != TEAM_OF(seat)))
		return ballOwner;
	int best = -1;
	long bestD = 0x7FFFFFFF;
	for (int i = 0; i < 4; i++)
	{
		if (i == seat)
			continue;
		if (fourMode == FOUR_2V2 && TEAM_OF(i) == TEAM_OF(seat))
			continue;
		long dx = (long)ent[i]->xPos - ent[seat]->xPos;
		long dz = (long)ent[i]->zPos - ent[seat]->zPos;
		long d = dx * dx + dz * dz;
		if (d < bestD)
		{
			bestD = d;
			best = i;
		}
	}
	/* 2v2: if the partner AI already covers this target, take the other one */
	if (fourMode == FOUR_2V2 && best >= 0)
	{
		int partner = seat ^ 2;
		if (aiTarget[partner] == best && ent[partner]->persona != kHumanPlayer)
		{
			int other = best ^ 2;
			if (other != seat && ent[other]->mode == kInArena)
				best = other;
		}
	}
	return best < 0 ? (seat ^ 1) : best;
}

/* z-mirror of a facing octal (swap north<->south, keep east/west and rested);
 * boardForceTable is exactly antisymmetric under this, so the FFA-4 field can be
 * reflected in z to make the north-only persona AI drive a southern-goal seat. */
static short zmirFacing (short d)
{
	return (d == kFacingRested) ? d : (short)((4 - d) & 7);
}

static void ai4Decide (int seat)
{
	int target = aiTarget[seat];
	if (ballOwner != lastToucher || target < 0 || (Ticks % 90) == (seat * 17) % 90)
		target = pickTarget(seat);
	aiTarget[seat] = target;

	/* pair view: the persona code reads the two 2-player globals directly */
	playerType savedP = thePlayer, savedO = theOpponent;
	short savedWho = whosGotBall;
	short savedMod = theBall.modifier;
	char savedBallMode = theBall.mode;
	short savedSel = ent[seat]->selector;

	thePlayer = *ent[target];
	theOpponent = *ent[seat];
	/* the acting AI seat is always presented as "the opponent" and its target as
	 * "the player" — the roles the 1992 persona code keys off via ->selector to
	 * read possession. Seats set up with kPlayerSelector (0 and 2) would
	 * otherwise misread whosGotBall and either freeze while holding the ball
	 * (never scoring) or repeatedly re-part a ball they don't own. The real
	 * selector is restored on the decided copy below so nothing persists. */
	theOpponent.selector = kOpponentSelector;
	thePlayer.selector = kPlayerSelector;
	/* Aim the AI at the goal it currently owns. FFA-2 goals rotate owners; FFA-4
	 * goals are fixed but two of them are southern. whichGoal encodes only the
	 * left/right side (non-owner FFA-2 seats keep their default side). */
	int ownedGoal = -1, mirror = 0;
	int nGoals = (fourMode == FOUR_FFA4) ? 4 : (fourMode == FOUR_FFA2) ? 2 : 0;
	for (int g = 0; g < nGoals; g++)
		if (goalOwner[g] == seat) { ownedGoal = g; break; }
	if (ownedGoal >= 0)
		theOpponent.whichGoal = (ownedGoal & 1) ? kRightGoal : kLeftGoal;

	/* FFA-4 southern goals (g = 2 SW, 3 SE) live at z < 0, which the verbatim
	 * goal-seek/shoot logic (all gated on z > 0) can't target. Present a world
	 * reflected in z for the decision, then un-mirror the result below; the
	 * FFA-4 field is z-symmetric so the north-only AI drives the seat south. */
	mirror = (fourMode == FOUR_FFA4 && ownedGoal >= 2);
	if (mirror)
	{
		theOpponent.zPos = -theOpponent.zPos; theOpponent.zVel = -theOpponent.zVel;
		theOpponent.direction = zmirFacing(theOpponent.direction);
		thePlayer.zPos = -thePlayer.zPos; thePlayer.zVel = -thePlayer.zVel;
		thePlayer.direction = zmirFacing(thePlayer.direction);
		theBall.zPos = -theBall.zPos; theBall.zVel = -theBall.zVel;
	}

	if (ballOwner == seat)
	{
		whosGotBall = kOpponentHasBall;
		theBall.modifier = kOpponentHolding;
	}
	else if (ballOwner == target)
	{
		whosGotBall = kPlayerHasBall;
		theBall.modifier = kPlayerHolding;
	}
	else
	{
		whosGotBall = (theBall.mode == kBallRolling) ? kBallRollsFreely : kBallIsNotHere;
		theBall.modifier = kNoOneLastHeld;
	}

	OpponentDecides(&theOpponent);      /* verbatim persona */

	playerType decided = theOpponent;
	decided.selector = savedSel;            /* don't persist the forced pair-view role */
	char ballModeAfter = theBall.mode;

	if (mirror)
	{
		/* un-mirror the decision back into real (southern) space, and the ball,
		 * which the persona may have repositioned when it shot (DoPersonBallParted
		 * wrote mirror-space values). Negating restores an untouched ball exactly. */
		decided.zPos = -decided.zPos; decided.zVel = -decided.zVel;
		decided.direction = zmirFacing(decided.direction);
		theBall.zPos = -theBall.zPos; theBall.zVel = -theBall.zVel;
		if (savedBallMode == kBallHeld && ballModeAfter == kBallRolling)
			BallRectFromPosition();     /* recompute the rect at the real position */
	}

	thePlayer = savedP;
	theOpponent = savedO;
	whosGotBall = savedWho;
	theBall.modifier = savedMod;
	*ent[seat] = decided;

	/* the persona may have thrown the ball (DoPersonBallParted inside the view) */
	if (savedBallMode == kBallHeld && ballModeAfter == kBallRolling && ballOwner == seat)
	{
		ballOwner = -1;
		lastToucher = seat;
		syncLegacyBallState();
		if (fourMode == FOUR_2V2)
			UpdateArrows();
	}
}

/* ---------------------------------------------------------------- scoring / HUD */

static void draw4Text (short x, short y, const char *s, unsigned char colorIdx)
{
	Str255 ps;
	size_t n = strlen(s);
	if (n > 255) n = 255;
	ps[0] = (unsigned char)n;
	memcpy(ps + 1, s, n);
	RGBColor c;
	Index2Color(colorIdx, &c);
	RGBForeColor(&c);
	MoveTo(x, y);
	DrawString(ps);
}

/* copy a HUD rect into work+back so the dirty-rect renderer can't erase it */
static void pinToBuffers (const Rect *r)
{
	CopyBits(&(((GrafPtr)mainWndo)->portBits), &((GrafPtr)offCWorkPtr)->portBits,
	         r, r, srcCopy, kNilPointer);
	CopyBits(&(((GrafPtr)mainWndo)->portBits), &((GrafPtr)offCBackPtr)->portBits,
	         r, r, srcCopy, kNilPointer);
}

static void drawChip (int seat)
{
	GrafPtr wasPort;
	Rect r;
	char buf[16];
	static const short chipX[4] = { 8, 96, 464, 552 };

	r.left = chipX[seat];
	r.right = (short)(r.left + 76);
	r.top = 4;
	r.bottom = 24;

	GetPort(&wasPort);
	SetPort((GrafPtr)mainWndo);
	PmForeColor(15);
	PaintRect(&r);
	RGBColor c;
	Index2Color(seatColor[seat], &c);
	RGBForeColor(&c);
	FrameRect(&r);
	snprintf(buf, sizeof buf, "P%d %2d", seat + 1, score4[seat]);
	draw4Text((short)(r.left + 8), (short)(r.bottom - 6), buf, seatColor[seat]);
	Index2Color(15, &c);
	RGBForeColor(&c);                   /* verbatim code assumes a black pen */
	SetPort(wasPort);
	pinToBuffers(&r);
}

static void drawAllChips (void)
{
	for (int i = 0; i < 4; i++)
		drawChip(i);
}

static void draw4Plates (void)
{
	/* 2v2 name plates over the scoreboard labels */
	GrafPtr wasPort;
	Rect l, r;
	SetRect(&l, 0, 0, 69, 9);
	SetRect(&r, 0, 0, 69, 9);
	OffsetRect(&l, 25, 426);
	OffsetRect(&r, 553, 426);
	GetPort(&wasPort);
	SetPort((GrafPtr)mainWndo);
	PmForeColor(15);
	PaintRect(&l);
	PaintRect(&r);
	draw4Text((short)(l.left + 2), (short)(l.bottom), "TEAM A", 8);
	draw4Text((short)(r.left + 2), (short)(r.bottom), "TEAM B", 7);
	{
		RGBColor c;
		Index2Color(15, &c);
		RGBForeColor(&c);
	}
	SetPort(wasPort);
	pinToBuffers(&l);
	pinToBuffers(&r);
}

/* ---- goal ownership bands (modes B and C) ----
 * Same technique as UpdateGoalPicts: a polygon region along the rim, carved
 * to the league's goal width, painted into the back buffer then copied. */

static const short rimPts[7][2] = {
	{12,250},{22,203},{53,158},{102,119},{166,89},{240,70},{320,64}
};
/* league strips (large arena): outer A..D minus inner B..C */
static const short leagueStrips[5][4] = {
	{20,246,47,174}, {25,230,58,158}, {32,208,67,145}, {42,190,80,125}, {50,170,96,110}
};
#define BAND_H 20
#define SOUTH_MIRROR 524   /* y' = SOUTH_MIRROR - y approximates the near rim */

static void paintGoalBand (int g, unsigned char colorIdx)
{
	/* g: 0=NW 1=NE 2=SW 3=SE */
	int east = (g == 1 || g == 3);
	int south = (g >= 2);
	GrafPtr wasPort;
	RgnHandle rgn = NewRgn(), tmp = NewRgn();
	RGBColor c;

	GetPort(&wasPort);
	SetPort((GrafPtr)offCBackPtr);

	OpenRgn();
	for (int i = 0; i < 7; i++)
	{
		short x = rimPts[i][0], y = rimPts[i][1];
		if (east) x = (short)(640 - x);
		if (south) y = (short)(SOUTH_MIRROR - y);
		if (i == 0) MoveTo(x, y); else LineTo(x, y);
	}
	for (int i = 6; i >= 0; i--)
	{
		short x = rimPts[i][0];
		short y = (short)(rimPts[i][1] - BAND_H);
		if (east) x = (short)(640 - x);
		if (south) y = (short)(SOUTH_MIRROR - rimPts[i][1] + BAND_H);
		if (i == 6) LineTo(x, y); else LineTo(x, y);
	}
	{
		short x = rimPts[0][0], y = rimPts[0][1];
		if (east) x = (short)(640 - x);
		if (south) y = (short)(SOUTH_MIRROR - y);
		LineTo(x, y);
	}
	CloseRgn(rgn);

	const short *st = leagueStrips[isLeague >= 0 && isLeague <= 4 ? isLeague : 4];
	short a = st[0], d = st[1], b = st[2], cc = st[3];
	if (east)
	{
		SetRectRgn(tmp, (short)(640 - d), 0, (short)(640 - a), 480);
		SectRgn(rgn, tmp, rgn);
		SetRectRgn(tmp, (short)(640 - cc), 0, (short)(640 - b), 480);
		DiffRgn(rgn, tmp, rgn);
	}
	else
	{
		SetRectRgn(tmp, a, 0, d, 480);
		SectRgn(rgn, tmp, rgn);
		SetRectRgn(tmp, b, 0, cc, 480);
		DiffRgn(rgn, tmp, rgn);
	}

	Index2Color(1, &c);                  /* yellow halo, like the original */
	RGBForeColor(&c);
	OffsetRgn(rgn, (short)(east ? 1 : -1), -1);
	PaintRgn(rgn);
	OffsetRgn(rgn, (short)(east ? -1 : 1), 1);
	Index2Color(colorIdx, &c);
	RGBForeColor(&c);
	PaintRgn(rgn);
	Index2Color(15, &c);
	RGBForeColor(&c);

	/* copy the painted area back->work->screen */
	{
		Rect box;
		box.left = east ? (short)(640 - 246) : 8;
		box.right = east ? 632 : 250;
		box.top = south ? (short)(SOUTH_MIRROR - 254) : 40;
		box.bottom = south ? (short)(SOUTH_MIRROR - 40) : 254;
		CopyBits(&((GrafPtr)offCBackPtr)->portBits, &((GrafPtr)offCWorkPtr)->portBits,
		         &box, &box, srcCopy, kNilPointer);
		CopyBits(&((GrafPtr)offCBackPtr)->portBits, &(((GrafPtr)mainWndo)->portBits),
		         &box, &box, srcCopy, kNilPointer);
	}

	DisposeRgn(rgn);
	DisposeRgn(tmp);
	SetPort(wasPort);
}

static void paintOwnedGoals (void)
{
	if (fourMode == FOUR_FFA2)
	{
		paintGoalBand(0, seatColor[goalOwner[0]]);
		paintGoalBand(1, seatColor[goalOwner[1]]);
	}
	else if (fourMode == FOUR_FFA4)
	{
		for (int g = 0; g < 4; g++)
			paintGoalBand(g, seatColor[goalOwner[g]]);
	}
}

static void endFourGame (int winnerSeat);

static void score4Point (int seat)
{
	lengthOfApplause = 120;
	StartApplauseSound();
	PlaySoundSMS(kScoreSound);
	score4[seat]++;
	drawChip(seat);
	if (score4[seat] >= FFA_WIN)
	{
		fourDone = 1;
		fourWinner = seat;
	}
}

static void reset4Ball (void)
{
	ResetBall();                        /* verbatim: firing state, door, timers */
	for (int i = 0; i < 4; i++)
	{
		ent[i]->mouseWasLetUp = TRUE;
		ent[i]->loopsBallHeld = 0;
	}
	ballOwner = -1;
	lastToucher = -1;                    /* nobody has touched the new ball yet —
	                                     * avoids a spurious foul against the prior
	                                     * toucher if the reset ball rolls out */
	syncLegacyBallState();
}

static void ball4InGoal (void)
{
	int g = goalAtBall();
	theBall.eraseTheBall = TRUE;

	if (fourMode == FOUR_2V2)
	{
		/* the goal scores for the team that owns it, verbatim path */
		if (g == 0)
			DoPlayerScores();
		else
			DoOpponentScores();
		reset4Ball();
		return;
	}
	score4Point(goalOwner[g]);
	if (fourMode == FOUR_FFA2)
	{
		/* a scored-on goal rotates immediately */
		goalOwner[g] = (goalOwner[g] + 1) & 3;
		if (goalOwner[0] == goalOwner[1])   /* never let one seat own both goals */
			goalOwner[g] = (goalOwner[g] + 1) & 3;
		paintGoalBand(g, seatColor[goalOwner[g]]);
	}
	reset4Ball();
}

static void foul4 (int seat)
{
	if (fourMode == FOUR_2V2)
	{
		if (TEAM_OF(seat) == 0)
			HandlePlayerFoul();             /* verbatim team fouls + criticals + sounds */
		else
			HandleOpponentFoul();
		return;
	}
	lengthOfMob = 90;
	StartMobSound();
	QueueUpIncidental(kFoulSound, 6, 100);
	fouls4[seat]++;
	if (fouls4[seat] >= kCriticalFoul)
	{
		fouls4[seat] = 0;
		if (score4[seat] > 0)
		{
			score4[seat]--;                 /* FFA critical: lose a point */
			drawChip(seat);
		}
	}
}

/* ---------------------------------------------------------------- persons */

static void handle4Person (int seat)
{
	playerType *who = ent[seat];

	switch (who->mode)
	{
		case kInArena:
			switch (who->flag)
			{
				case kIsNormal:
				case kIsRebounding:
					if (who->persona == kHumanPlayer)
						apply4HumanForces(seat);
					else
						ai4Decide(seat);
					move4Person(who);
					break;
				case kIsOutOfBounds:
					if (ballOwner == seat)
					{
						foul4(seat);
						theBall.eraseTheBall = TRUE;
						reset4Ball();
					}
					StartPersonBeamOut(who);
					break;
			}
			if (ballOwner == seat && who->loopsBallHeld != 0)
			{
				who->loopsBallHeld--;
				if (fourMode == FOUR_2V2)
					UpdateBallTimers(who);
				if (who->loopsBallHeld == 120)
					PlaySoundSMS(kHoldingSound);
				else if (who->loopsBallHeld == 0)
				{
					foul4(seat);
					theBall.eraseTheBall = TRUE;
					reset4Ball();
				}
			}
			who->dirFlagSrc = who->direction;
			who->postFlagSrc = who->posture;
			who->arrayFlagMask = kMask;
			who->dirFlagMask = who->direction;
			who->postFlagMask = who->posture;
			break;
		case kBeamingIn:
			DoPersonBeamingIn(who);          /* verbatim */
			break;
		case kBeamingOut:
			DoPersonBeamingOut(who);         /* verbatim (ResetPerson on finish) */
			break;
		case kInStasis:
			DoPersonInStasis(who);           /* verbatim */
			break;
	}
}

/* ---------------------------------------------------------------- rendering */

/* CopyMask with a palette remap (used to give seats 2/3 their own colors) */
static void copyMaskRemap (const BitMap *srcBits, const BitMap *maskBits,
                           const BitMap *dstBits, const Rect *srcRect,
                           const Rect *maskRect, const Rect *dstRect,
                           const unsigned char *remap)
{
	int w = srcRect->right - srcRect->left;
	int h = srcRect->bottom - srcRect->top;
	int sx = srcRect->left, sy = srcRect->top;
	int mx = maskRect->left, my = maskRect->top;
	int dx = dstRect->left, dy = dstRect->top;
	int d;

	if (sx < 0) { d = -sx; sx += d; mx += d; dx += d; w -= d; }
	if (sy < 0) { d = -sy; sy += d; my += d; dy += d; h -= d; }
	if (dx < 0) { d = -dx; dx += d; sx += d; mx += d; w -= d; }
	if (dy < 0) { d = -dy; dy += d; sy += d; my += d; h -= d; }
	if (sx + w > srcBits->bounds.right)  w = srcBits->bounds.right - sx;
	if (sy + h > srcBits->bounds.bottom) h = srcBits->bounds.bottom - sy;
	if (mx + w > maskBits->bounds.right)  w = maskBits->bounds.right - mx;
	if (my + h > maskBits->bounds.bottom) h = maskBits->bounds.bottom - my;
	if (dx + w > dstBits->bounds.right)  w = dstBits->bounds.right - dx;
	if (dy + h > dstBits->bounds.bottom) h = dstBits->bounds.bottom - dy;
	if (w <= 0 || h <= 0)
		return;

	for (int y = 0; y < h; y++)
	{
		const uint8_t *s = (const uint8_t *)srcBits->baseAddr + (long)(sy + y) * srcBits->rowBytes + sx;
		const uint8_t *m = (const uint8_t *)maskBits->baseAddr + (long)(my + y) * maskBits->rowBytes + mx;
		uint8_t *dd = (uint8_t *)dstBits->baseAddr + (long)(dy + y) * dstBits->rowBytes + dx;
		for (int x = 0; x < w; x++)
			if (m[x])
				dd[x] = remap ? remap[s[x] & 15] : s[x];
	}
}

static void fillIn4Rects (int seat)
{
	playerType *who = ent[seat];
	if (seatBank[seat] == 0)
		who->srcRect = playerSrcRects[(int)who->dirFlagSrc][(int)who->postFlagSrc];
	else
		who->srcRect = opponentSrcRects[(int)who->dirFlagSrc][(int)who->postFlagSrc];
	if (who->arrayFlagMask == kMask)
		who->maskRect = playerSrcRects[(int)who->dirFlagMask][(int)who->postFlagMask];
	else
		who->maskRect = fadeMaskRects[(int)who->dirFlagMask][(int)who->postFlagMask];
}

static const unsigned char *seatRemap (int seat)
{
	if (fourMode == FOUR_2V2)
		return NULL;
	if (seat == 2) return remapSeat2;
	if (seat == 3) return remapSeat3;
	return NULL;
}

/* ---- direct 8bpp helpers for the FFA overlays (goal circles, ball marker) ---- */
#define GOAL_CIRCLE_IDX 3         /* Apple-palette red (0xDD0806) */

static void plotPix (const BitMap *bm, unsigned char idx, int x, int y)
{
	if (x < bm->bounds.left || x >= bm->bounds.right ||
	    y < bm->bounds.top  || y >= bm->bounds.bottom) return;
	*((uint8_t *)bm->baseAddr + (long)(y - bm->bounds.top) * bm->rowBytes
	  + (x - bm->bounds.left)) = idx;
}

static unsigned char peekBack (int x, int y)
{
	const BitMap *bm = &((GrafPtr)offCBackPtr)->portBits;
	if (x < bm->bounds.left || x >= bm->bounds.right ||
	    y < bm->bounds.top  || y >= bm->bounds.bottom) return 0;
	return *((const uint8_t *)bm->baseAddr + (long)(y - bm->bounds.top) * bm->rowBytes
	         + (x - bm->bounds.left));
}

/* filled downward-pointing triangle (apex at the bottom, toward the player) */
static void drawDownTri (const BitMap *dst, int cx, int top, int w, int h, unsigned char idx)
{
	for (int r = 0; r < h; r++)
	{
		int half = (w / 2) * (h - 1 - r) / (h - 1);
		for (int x = cx - half; x <= cx + half; x++)
			plotPix(dst, idx, x, top + r);
	}
}

/* The two corner scoreboards (InitDigiDispData's 13-inch dest rects, plus their
 * PICT frames) sit in the lower corners where the mirrored south goal circles
 * land. The score counter must stay on top of the goal, so skip goal-circle
 * pixels that fall inside either scoreboard frame. */
static int inCornerScoreboard (int x, int y)
{
	if (y < 419 || y > 469)
		return 0;
	return (x >= 17 && x <= 98) || (x >= 542 && x <= 623);
}

/* The arena PICT only carries the two NORTHERN goal-circle marks; mirror them to
 * the FFA-4 southern goals (reflection about the arena z-axis, SOUTH_MIRROR). */
static void paint4SouthGoalCircles (void)
{
	static const short cx[2] = { 96, 544 };   /* large-arena north circle centers */
	const BitMap *back   = &((GrafPtr)offCBackPtr)->portBits;
	const BitMap *work   = &((GrafPtr)offCWorkPtr)->portBits;
	const BitMap *screen = &(((GrafPtr)mainWndo)->portBits);
	for (int i = 0; i < 2; i++)
	{
		for (int y = 86; y <= 122; y++)
			for (int x = cx[i] - 16; x <= cx[i] + 16; x++)
				if (peekBack(x, y) == GOAL_CIRCLE_IDX && !inCornerScoreboard(x, SOUTH_MIRROR - y))
					plotPix(back, GOAL_CIRCLE_IDX, x, SOUTH_MIRROR - y);
		Rect box;
		box.left = (short)(cx[i] - 16);          box.right  = (short)(cx[i] + 17);
		box.top  = (short)(SOUTH_MIRROR - 122);  box.bottom = (short)(SOUTH_MIRROR - 85);
		CopyBits(back, work,   &box, &box, srcCopy, kNilPointer);
		CopyBits(back, screen, &box, &box, srcCopy, kNilPointer);
	}
}

static int humanSeat (int seat)
{
	return ent[seat]->persona == kHumanPlayer;
}

/* Head overlay above a character: a "P#" plate for human seats and a caret for
 * whoever holds the ball. Returns 0 (empty) when the seat shows neither. When a
 * human holds the ball the caret sits above the plate, otherwise right on the
 * head. `out` receives the union bounding box. `holds` is passed in because the
 * four-player compositor tracks possession via ballOwner while the verbatim 1v1
 * path reads it from the person's posture. Suppressed entirely in classic mode. */
static int headOverlayRect (int seat, int holds, Rect *out)
{
	if (classicMode)
		return 0;
	if (ent[seat]->mode != kInArena)
		return 0;
	int human = humanSeat(seat);
	if (!human && !holds)
		return 0;
	short cx  = (short)((ent[seat]->isRect.left + ent[seat]->isRect.right) / 2);
	short top = ent[seat]->isRect.top;
	SetRect(out, cx, (short)(top - 2), cx, (short)(top - 2));
	if (human)
	{
		/* wide enough for a 3-char name plate */
		Rect r; SetRect(&r, (short)(cx - 16), (short)(top - 13), (short)(cx + 16), (short)(top - 2));
		UnionRect(out, &r, out);
	}
	if (holds)
	{
		short ct = human ? (short)(top - 25) : (short)(top - 13);
		Rect r; SetRect(&r, (short)(cx - 8), ct, (short)(cx + 9), (short)(ct + 10));
		UnionRect(out, &r, out);
	}
	return 1;
}

static void drawHeadOverlay (int seat, int holds, const BitMap *work)
{
	short cx  = (short)((ent[seat]->isRect.left + ent[seat]->isRect.right) / 2);
	short top = ent[seat]->isRect.top;
	int human = humanSeat(seat);

	if (holds)
	{
		short ct = human ? (short)(top - 25) : (short)(top - 13);
		drawDownTri(work, cx, (short)(ct + 1), 15, 9, 0 /* white outline */);
		drawDownTri(work, cx, (short)(ct + 2), 11, 7, seatColor[seat]);
	}
	if (human)
	{
		/* the player's name plate: black fill, badge-coloured border and text. The
		 * name (up to 3 chars) and colour come from the pre-match setup card. */
		unsigned char badge = PortPlayerColor(seat);
		const char *nm = PortPlayerName(seat);
		short l = (short)(cx - 16), r = (short)(cx + 16);
		short t = (short)(top - 13), b = (short)(top - 3);
		for (short y = t; y <= b; y++)
			for (short x = l; x <= r; x++)
				plotPix(work, 15 /* black */, x, y);
		for (short x = l; x <= r; x++) { plotPix(work, badge, x, t); plotPix(work, badge, x, b); }
		for (short y = t; y <= b; y++) { plotPix(work, badge, l, y); plotPix(work, badge, r, y); }
		{
			GrafPtr wasPort;
			RGBColor c;
			Str255 ps;
			int n = 0;
			for (; n < 3 && nm && nm[n]; n++) ps[1 + n] = (unsigned char)nm[n];
			ps[0] = (unsigned char)n;
			GetPort(&wasPort);
			SetPort((GrafPtr)offCWorkPtr);
			Index2Color(badge, &c);
			RGBForeColor(&c);
			MoveTo((short)(cx - 3 * n), (short)(top - 5));   /* ~6px/char, centred */
			DrawString(ps);
			Index2Color(15, &c);
			RGBForeColor(&c);           /* leave a black pen for the verbatim code */
			SetPort(wasPort);
		}
	}
}

/* ------------------------------------------------------------------ 1v1 HUD */
/* The single-player game renders through the verbatim RenderScene, which has no
 * seam for the head overlays. So we run the same P#/caret pass here, hooked from
 * HandlePostGraphics() (below) after the frame is composited: draw into the work
 * map, blit to the screen, then plug the holes — exactly as render4Scene does.
 * Possession comes from the ball (mode/modifier) since 1v1 has no ballOwner.
 * Seats 0/1 are thePlayer/theOpponent (ent[0]/ent[1]). Idle in classic mode. */
void PortSPHeadOverlays (void)
{
	static Rect hWas[2]   = { {0,0,0,0}, {0,0,0,0} };
	static int  hWasOn[2] = { 0, 0 };
	Rect hIs[2] = { {0,0,0,0}, {0,0,0,0} };
	int  hOn[2], holds[2];

	if (!drawThisFrame)
		return;

	/* 1v1 possession lives in the ball: mode kBallHeld + modifier says which
	 * side holds it (kPlayerHolding = thePlayer/seat 0, kOpponentHolding = 1). */
	int held = (theBall.mode == kBallHeld);
	holds[0] = held && (theBall.modifier == kPlayerHolding);
	holds[1] = held && (theBall.modifier == kOpponentHolding);
	for (int s = 0; s < 2; s++)
		hOn[s] = headOverlayRect(s, holds[s], &hIs[s]);
	if (!hOn[0] && !hOn[1] && !hWasOn[0] && !hWasOn[1])
		return;                                  /* nothing to draw or erase */

	const BitMap *work   = &((GrafPtr)offCWorkPtr)->portBits;
	const BitMap *back   = &((GrafPtr)offCBackPtr)->portBits;
	const BitMap *screen = &(((GrafPtr)mainWndo)->portBits);

	for (int s = 0; s < 2; s++)
		if (hOn[s])
			drawHeadOverlay(s, holds[s], work);

	/* publish each overlay (and erase its previous footprint) */
	for (int s = 0; s < 2; s++)
		if (hOn[s] || hWasOn[s])
		{
			Rect wm = hOn[s] ? hIs[s] : hWas[s];
			if (hOn[s] && hWasOn[s])
				UnionRect(&hIs[s], &hWas[s], &wm);
			CopyBits(work, screen, &wm, &wm, srcCopy, kNilPointer);
		}

	/* plug the holes back out of the work map */
	for (int s = 0; s < 2; s++)
		if (hOn[s])
			CopyBits(back, work, &hIs[s], &hIs[s], srcCopy, kNilPointer);

	for (int s = 0; s < 2; s++)
	{
		hWas[s]   = hIs[s];
		hWasOn[s] = hOn[s];
	}
}

/* Wrapper over the verbatim HandlePostGraphics (renamed to HandlePostGraphicsGame
 * for Render.c only, via a -D on that one file). RunStandardGame calls this every
 * frame right after RenderScene, giving the 1v1 HUD its per-frame hook without
 * touching any Sources/ file. */
void HandlePostGraphicsGame (void);
void HandlePostGraphics (void)
{
	HandlePostGraphicsGame();
	PortSPHeadOverlays();
	PortDrawMobileControls();
}

static void render4Scene (void)
{
	static Rect headWas[4] = { {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0} };
	static int  headWasOn[4] = { 0, 0, 0, 0 };
	Rect headIs[4] = { {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0} };
	int  headOn[4];
	for (int i = 0; i < 4; i++)
		headOn[i] = drawThisFrame && headOverlayRect(i, ballOwner == i, &headIs[i]);
	const BitMap *parts = &((GrafPtr)offCPartsPtr)->portBits;
	const BitMap *work = &((GrafPtr)offCWorkPtr)->portBits;
	const BitMap *back = &((GrafPtr)offCBackPtr)->portBits;
	const BitMap *screen = &(((GrafPtr)mainWndo)->portBits);
	Rect whole[4], wholeBall, wholeCursor;
	int order[4] = { 0, 1, 2, 3 };

	for (int i = 0; i < 4; i++)
		UnionRect(&ent[i]->isRect, &ent[i]->wasRect, &whole[i]);
	UnionRect(&theBall.isRect, &theBall.wasRect, &wholeBall);
	UnionRect(&boardCursor.isRect, &boardCursor.wasRect, &wholeCursor);

	/* z-sort back to front (farthest = biggest zPos first) */
	for (int i = 1; i < 4; i++)
	{
		int k = order[i], j = i - 1;
		while (j >= 0 && ent[order[j]]->zPos < ent[k]->zPos)
		{
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = k;
	}

	if (theDoor.stateChanged)
	{
		CopyBits(parts, work, &theDoor.srcRects[(int)theDoor.doorOpen][(int)theDoor.doorState],
		         &theDoor.destRects[(int)theDoor.doorOpen], srcCopy, kNilPointer);
		CopyBits(work, back, &theDoor.destRects[(int)theDoor.doorOpen],
		         &theDoor.destRects[(int)theDoor.doorOpen], srcCopy, kNilPointer);
	}

	if (showBoardCursor && !disableBoardCursor && drawThisFrame)
		CopyMask(parts, &offMaskMap, work, &boardCursor.srcRect,
		         &boardCursor.srcRect, &boardCursor.isRect);

	if ((theBall.mode == kBallRolling) && (!theBall.dontDraw) && drawThisFrame)
		CopyMask(parts, &offMaskMap, work, &theBall.srcRect,
		         &theBall.srcRect, &theBall.isRect);

	for (int oi = 0; oi < 4; oi++)
	{
		int i = order[oi];
		if (ent[i]->mode != kInStasis && drawThisFrame)
			copyMaskRemap(parts, &offMaskMap, work, &ent[i]->srcRect,
			              &ent[i]->maskRect, &ent[i]->isRect, seatRemap(i));
	}

	/* head overlays (P# plates for humans, ball caret for the holder), on top */
	for (int i = 0; i < 4; i++)
		if (headOn[i])
			drawHeadOverlay(i, ballOwner == i, work);

	for (int i = 0; i < 4; i++)
		if (ent[i]->mode != kInStasis && drawThisFrame)
			CopyBits(work, screen, &whole[i], &whole[i], srcCopy, kNilPointer);

	if ((theBall.mode == kBallRolling) && drawThisFrame)
		CopyBits(work, screen, &wholeBall, &wholeBall, srcCopy, kNilPointer);
	else if (theBall.eraseTheBall)
	{
		CopyBits(work, screen, &theBall.eraser, &theBall.eraser, srcCopy, kNilPointer);
		theBall.eraseTheBall = FALSE;
	}

	if (showBoardCursor && !disableBoardCursor && drawThisFrame)
		CopyBits(work, screen, &wholeCursor, &wholeCursor, srcCopy, kNilPointer);

	/* publish each head overlay (and erase its previous position) on top */
	if (drawThisFrame)
		for (int i = 0; i < 4; i++)
			if (headOn[i] || headWasOn[i])
			{
				Rect wm = headOn[i] ? headIs[i] : headWas[i];
				if (headOn[i] && headWasOn[i])
					UnionRect(&headIs[i], &headWas[i], &wm);
				CopyBits(work, screen, &wm, &wm, srcCopy, kNilPointer);
			}

	if (theDoor.stateChanged)
	{
		CopyBits(work, screen, &theDoor.destRects[(int)theDoor.doorOpen],
		         &theDoor.destRects[(int)theDoor.doorOpen], srcCopy, kNilPointer);
		CopyBits(parts, back, &theDoor.srcRects[(int)theDoor.doorOpen][(int)theDoor.doorState],
		         &theDoor.destRects[(int)theDoor.doorOpen], srcCopy, kNilPointer);
		CopyBits(work, back, &theDoor.destRects[(int)theDoor.doorOpen],
		         &theDoor.destRects[(int)theDoor.doorOpen], srcCopy, kNilPointer);
	}

	/* plug the holes on the work map */
	for (int i = 0; i < 4; i++)
		if (ent[i]->mode != kInStasis && drawThisFrame)
			CopyBits(back, work, &ent[i]->isRect, &ent[i]->isRect, srcCopy, kNilPointer);
	if ((theBall.mode == kBallRolling) && drawThisFrame)
		CopyBits(back, work, &theBall.isRect, &theBall.isRect, srcCopy, kNilPointer);
	if (showBoardCursor && !disableBoardCursor && drawThisFrame)
		CopyBits(back, work, &boardCursor.isRect, &boardCursor.isRect, srcCopy, kNilPointer);
	for (int i = 0; i < 4; i++)
		if (headOn[i])
			CopyBits(back, work, &headIs[i], &headIs[i], srcCopy, kNilPointer);

	if (drawThisFrame)
		for (int i = 0; i < 4; i++)
		{
			headWas[i] = headIs[i];
			headWasOn[i] = headOn[i];
		}

	PortDrawMobileControls();
}

/* ---------------------------------------------------------------- ball loop */

static void handle4Ball (void)
{
	switch (theBall.mode)
	{
		case kBallFiring:
			DoBallFiring();                  /* verbatim: door + launch */
			if (theBall.mode == kBallRolling)
				syncLegacyBallState();
			break;
		case kBallRolling:
			switch (theBall.flag)
			{
				case kIsNormal:
				case kIsRebounding:
					move4Ball();
					theBall.loopsBallIdle--;
					if (theBall.loopsBallIdle == 120)
						PlaySoundSMS(kIdleSound);
					if (theBall.loopsBallIdle == 0)
					{
						theBall.eraseTheBall = TRUE;
						reset4Ball();
					}
					break;
				case kIsOutOfBounds:
					if (lastToucher >= 0)
						foul4(lastToucher);
					theBall.eraseTheBall = TRUE;
					reset4Ball();
					break;
				case kIsInGoal:
					ball4InGoal();
					break;
			}
			break;
		default:
			break;
	}
	if (theDoor.phase != 0)
	{
		theDoor.phase--;
		if (theDoor.phase == 0)
		{
			theDoor.doorState = kDoorIsClosed;
			theDoor.stateChanged = TRUE;
		}
	}
}

/* ---------------------------------------------------------------- game flow */

static void seat4Setup (int mode, const int personas[4])
{
	static const short initX[4] = { kLgInitLeftXPos, kLgInitRightXPos,
	                                kLgInitLeftXPos, kLgInitRightXPos };
	static const short initZ2v2[4] = { 1800, 1800, -1800, -1800 };
	static const short initZFFA[4] = { 0, 0, 0, 0 };
	static const short initXFFA[4] = { kLgInitLeftXPos, kLgInitRightXPos, -1536, 1536 };
	static const short initZFFAo[4] = { 0, 0, 2600, 2600 };
	int nextPad = 1;

	for (int i = 0; i < 4; i++)
	{
		playerType *p = ent[i];
		p->persona = (short)personas[i];
		p->selector = TEAM_OF(i) == 0 ? kPlayerSelector : kOpponentSelector;
		p->whichGoal = TEAM_OF(i) == 0 ? kLeftGoal : kRightGoal;
		p->teaksThresh = 50;
		p->strategy = kRunDiagonal;
		if (mode == FOUR_2V2)
		{
			p->initXPos = initX[i];
			p->initZPos = initZ2v2[i];
		}
		else
		{
			p->initXPos = i < 2 ? initX[i] : initXFFA[i];
			p->initZPos = i < 2 ? initZFFA[i] : initZFFAo[i];
		}
		/* P1 drives keyboard/mouse AND the first pad (slot 0); later human seats
		 * take the remaining pads 1,2,3 (seatSlot n reads pad n-1) */
		seatSlot[i] = -1;
		if (personas[i] == kHumanPlayer)
			seatSlot[i] = (i == 0) ? 0 : (1 + nextPad++);
		seatBank[i] = TEAM_OF(i);
		aiTarget[i] = -1;
		score4[i] = 0;
		fouls4[i] = 0;
		ResetPerson(p);
		p->wasRect = p->isRect;
	}
}

static void check4AbortiveInput (void)
{
	static int pausePrev = 0;
	int pauseNow;
	GetKeys(theKeyMap);
	if (BitTst(&theKeyMap, kCommandKeyMap))
	{
		if (BitTst(&theKeyMap, kQKeyMap))
		{
			quitting = TRUE;
			fourDone = 1;
			fourWinner = -1;
		}
		if (BitTst(&theKeyMap, kEKeyMap))
		{
			fourDone = 1;
			fourWinner = -1;
		}
	}
	if (BitTst(&theKeyMap, kSKeyMap))
		DoSoundToggle();                     /* verbatim */

	/* Pause on a fresh Esc/Start press only (edge-triggered) — otherwise the
	 * same press that resumes would immediately re-pause. */
	pauseNow = BitTst(&theKeyMap, kTabKeyMap) ? 1 : 0;
	if (pauseNow && !pausePrev)
	{
		/* pause screen: resume on a fresh Esc/Start; end with E (keyboard) or
		 * the pad's Back/View button. Wait for the pause key to be released
		 * first so the entry press can't immediately resume or end. */
		long pausedAt = Ticks;
		int armed = 0;
		DrawPauseScreen();
		ShimForcePresent();
		shimInput.tapFresh = 0;                      /* ignore any pending tap */
		shimInput.backEdge = 0;                      /* the Back press that opened this is spent */
		shimInput.pauseTap = 0;
		for (;;)
		{
			GetKeys(theKeyMap);
			if (shimInput.quitRequested)
			{
				quitting = TRUE;
				fourDone = 1;
				fourWinner = -1;
				break;
			}
			if (shimInput.backEdge)                  /* Android: a second Back press ends the game */
			{
				shimInput.backEdge = 0;
				fourDone = 1;
				fourWinner = -1;
				break;
			}
			if (shimInput.tapFresh)                  /* touch: tap anywhere resumes (Back ends) */
			{
				shimInput.tapFresh = 0;
				break;
			}
			if (armed && (BitTst(&theKeyMap, kEKeyMap) ||
			              ShimAnyPadButton(SDL_GAMEPAD_BUTTON_BACK)))   /* end game */
			{
				fourDone = 1;
				fourWinner = -1;
				break;
			}
			if (!BitTst(&theKeyMap, kTabKeyMap))
				armed = 1;
			else if (armed)
				break;                               /* resume */
			SDL_Delay(10);
		}
		/* drop any pause latch the resume tap itself set (a top-centre tap), so the
		 * game loop's next check4AbortiveInput doesn't instantly re-pause */
		shimInput.pauseTap = 0;
		shimInput.backEdge = 0;
		shimInput.tapFresh = 0;
		baseTime += (Ticks - pausedAt) / 60;
		RedrawWholeScreen();
		if (fourMode != FOUR_2V2)
			drawAllChips();
		if (fourMode == FOUR_FFA4)
			paint4SouthGoalCircles();
	}
	pausePrev = pauseNow;
}

static void endFourGame (int winnerSeat)
{
	FlushSoundQueues();
	if (winnerSeat >= 0)
	{
		PlaySoundSMS(kBellSound);
		lengthOfApplause = kCrowdClosingHoopla;
		StartApplauseSound();

		/* losers beam out; give the winner a short hoopla lap */
		for (int i = 0; i < 4; i++)
			if (i != winnerSeat && ent[i]->mode == kInArena)
				StartPersonBeamOut(ent[i]);

		char msg[48];
		if (fourMode == FOUR_2V2)
			snprintf(msg, sizeof msg, "TEAM %s WINS!",
			         playerWonTheGame == kPlayerWon ? "A" : "B");
		else
			snprintf(msg, sizeof msg, "PLAYER %d WINS!", winnerSeat + 1);

		long until = Ticks + 300;
		long waitTil = Ticks + kTickDelay;
		while (Ticks < until && !quitting)
		{
			for (int i = 0; i < 4; i++)
				handle4Person(i);
			handle4Collisions();
			HandleCrowdSound();
			HandleIncidentalQueue();
			GetKeys(theKeyMap);
			if (shimInput.quitRequested)
				quitting = TRUE;
			while (Ticks < waitTil)
				GetKeys(theKeyMap);
			waitTil = Ticks + kTickDelay;
			for (int i = 0; i < 4; i++)
				fillIn4Rects(i);
			render4Scene();
			{
				GrafPtr wasPort;
				RGBColor blackC;
				GetPort(&wasPort);
				SetPort((GrafPtr)mainWndo);
				draw4Text((short)(screenWide / 2 - (short)(strlen(msg) * 4)), 40, msg, 1);
				Index2Color(15, &blackC);
				RGBForeColor(&blackC);
				SetPort(wasPort);
			}
			for (int i = 0; i < 4; i++)
				ent[i]->wasRect = ent[i]->isRect;
			theBall.eraser = theBall.isRect;
			theBall.wasRect = theBall.isRect;
		}
	}
	StopCrowdSound();
	TurnSMSOff();
	FlushSoundQueues();
}

void PortFourRun (int mode, const int personas[4])
{
	long waitTil;

	fourMode = mode;
	fourDone = 0;
	fourWinner = -1;
	ballOwner = -1;
	lastToucher = -1;

	if (splashIsUp)
	{
		DissolveWorkToMain();
		splashIsUp = FALSE;
	}

	savedCanReplay = canReplay;
	canReplay = FALSE;                     /* replay ring only knows 2 players */
	whichGame = kStandardGame;             /* 2v2 uses the standard rules */
	leftGoalLeague = isLeague;
	rightGoalLeague = isLeague;
	leftGoalIsPlayers = TRUE;
	netGameInSession = FALSE;
	gameIsOver = FALSE;
	playerWonTheGame = kNoOneWon;
	disableBoardCursor = (personas[0] != kHumanPlayer);

	seat4Setup(mode, personas);
	shimInput.splitInputs = (mode != FOUR_OFF);
	PortInputSetPlayMode(1);

	UpdateGoalPicts(FALSE);
	RefreshMainWindow();

	playerScore = 0;
	opponentScore = 0;
	playerFouls = 0;
	opponentFouls = 0;
	playerTotalGoals = opponentTotalGoals = 0;
	playerTotalFouls = opponentTotalFouls = 0;
	playerTotalCrits = opponentTotalCrits = 0;

	TurnSMSOn();
	FlushSoundQueues();
	StartCrowdSound();
	if (enableAnnouncer && !shimHeadless)
		DoOpeningAnnouncer();
	DisplayHoopla();

	reset4Ball();
	theBall.wasRect = theBall.isRect;

	UpdatePlayerScore();
	UpdateOpponentScore();
	DisplayPlayerFouls(0);
	DisplayOpponentFouls(0);
	if (mode == FOUR_2V2)
		draw4Plates();
	else
	{
		drawAllChips();
		if (mode == FOUR_FFA2)
		{
			goalOwner[0] = 0;
			goalOwner[1] = 1;
			nextSwitchTick = Ticks + 12 * 60;
			nextSwitchGoal = 0;
		}
		else
		{
			for (int g = 0; g < 4; g++)
				goalOwner[g] = g;
		}
		paintOwnedGoals();
		if (mode == FOUR_FFA4)
			paint4SouthGoalCircles();
	}

	baseTime = Ticks / 60;
	drawThisFrame = TRUE;
	waitTil = Ticks + kTickDelay;

	while (!fourDone)
	{
		DetermineFrameRate();

		for (int i = 0; i < 4; i++)
			handle4Person(i);
		HandleBoardCursor();
		handle4Ball();
		handle4Collisions();
		HandleCrowdSound();
		HandleIncidentalQueue();

		check4AbortiveInput();

		{
			short starsWinked = 0;
			while (Ticks < waitTil)
			{
				GetKeys(theKeyMap);
				if (starsWinked < 3)
				{
					TwinkleAStar();
					starsWinked++;
				}
			}
		}
		waitTil = Ticks + kTickDelay;

		for (int i = 0; i < 4; i++)
			fillIn4Rects(i);
		render4Scene();

		if (drawThisFrame)
		{
			theBall.eraser = theBall.isRect;
			theBall.wasRect = theBall.isRect;
			boardCursor.wasRect = boardCursor.isRect;
			for (int i = 0; i < 4; i++)
				ent[i]->wasRect = ent[i]->isRect;
		}

		/* mode B: rotate goal ownership on a staggered clock */
		if (fourMode == FOUR_FFA2 && Ticks >= nextSwitchTick)
		{
			int g = nextSwitchGoal;
			goalOwner[g] = (goalOwner[g] + 1) & 3;
			/* both goals may not belong to the same seat */
			if (goalOwner[0] == goalOwner[1])
				goalOwner[g] = (goalOwner[g] + 1) & 3;
			PlaySoundSMS(kBellSound);
			paintGoalBand(g, seatColor[goalOwner[g]]);
			nextSwitchGoal ^= 1;
			nextSwitchTick = Ticks + 12 * 60;
		}

		/* 2v2 game over comes from the verbatim scoring path */
		if (fourMode == FOUR_2V2 && gameIsOver)
		{
			fourDone = 1;
			fourWinner = (playerWonTheGame == kPlayerWon) ? 0 : 1;
		}
	}

	endFourGame(fourWinner);

	shimInput.splitInputs = 0;
	canReplay = savedCanReplay;
	fourMode = FOUR_OFF;
	primaryMode = kIdleMode;
	pausing = FALSE;
	PortInputSetPlayMode(0);
	RefreshMainWindow();
}
