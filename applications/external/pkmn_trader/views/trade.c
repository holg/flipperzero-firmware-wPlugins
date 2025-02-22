/*
 * This setup always forces the flipper to the follower/slave role in the link.
 * As far as I can tell, there is no specific reason for this other than it may
 * be a bit easier to handle an incoming clock rather than generating a clock.
 *
 * As documented here: http://www.adanscotney.com/2014/01/spoofing-pokemon-trades-with-stellaris.html
 * The general gist of the communication is as follows:
 * - Each gameboy tries to listen for an external clock coming in on the link cable.
 *   After some unknown timeout, this gameboy decides its going to take the leader/master role.
 *   In this state, it generates a clock and repeatedly sends out PKMN_MASTER(0x01)
 *   TODO: I'm not sure what kind of timeouts exist. Nor exactly how the GBs know they are connected.
 * - The other side, sensing a clock from the leader/master, then responds with PKMN_SLAVE(0x02)
 *
 *   In this application, we more or less force the flipper in to the follower/slave role. I'm
 *   not really sure why, but I assume it goes back to the original reference implementation.
 *   In the Flipper, it might also just be easier with the asynchronous context to be in the
 *   follower/slave role and just respond to clocks on an interrupt.
 *
 * - Once both sides understand their roles, they both respond with PKMN_BLANK(0x00)
 * - At this point, each gameboy repeatedly sends the menu item it has highlighted.
 *   These are ITEM_*_HIGHLIGHTED.
 * - Then, once both sides send ITEM_*_SELECTED, the next step occurs.
 *
 *   In this application, we simply repeat the same value back to the gameboy. That is,
 *   if the connected gameboy selected trade, we respond with trade as well.
 *
 * - Once the player on the gameboy side uses the trade table, a block of data is
 *   transmitted. This includes random bytes (presumably to set up the RNG seeds
 *   between two devices), and all trainer/pokemon data up from (this is the trade_block).
 * - At this point, both sides have full copies of each other's currenty party. The sides
 *   simply indicate which pokemon they are sending.
 *
 *   Interestingly, there is a close session byte (0x7f) that we don't seem to use at this time.
 *   Could be useful for, e.g. indicating to the flipper that we're done trading for a more
 *   clean exit.
 *
 *   Also, the above website mentions the data struct being 415 bytes, but we only receive
 *   405. The original Flipper implementation also used 405 bytes for the output. Finally,
 *   some other implementations of this that have surfaced use 418 bytes (with this original
 *   implementation having 3 bytes at the end of the struct commented out).
 *
 *   Doing the calculations myself, 415 should be the expected side of the trade block sent
 *   including the player name, appended with the 6-pokemon party structure:
 *   https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_data_structure_(Generation_I)#6-Pok.C3.A9mon_Party_Structure
 *   (note that all of the OT names and pokemon nicknames in the table actually are 11 bytes
 *   in memory)
 *
 *   Digging through some disassembled and commented pokemon code, it does appear that there are
 *   3 extra bytes sent. So the 418 number may be more correct. 
 *
 *   Seems like there are 9 random numbers according to comments in disassembled pokemon code? But it could also be 17 based on RN+RNS lengths?
 *
 *   Once that is sent, serial preamble length is sent
 *
 *   I think I need to hook this up to a logic analyzer to see more.
 */
#include <furi.h>
#include <furi_hal.h>

#include <gui/view.h>
#include <pokemon_icons.h>

#include "../pokemon_app.h"
#include "trade_patch_list.h"

#define GAME_BOY_CLK gpio_ext_pb2
#define GAME_BOY_SI gpio_ext_pc3
#define GAME_BOY_SO gpio_ext_pb3

#define DELAY_MICROSECONDS 15
#define PKMN_BLANK 0x00

#define ITEM_1_HIGHLIGHTED 0xD0
#define ITEM_2_HIGHLIGHTED 0xD1
#define ITEM_3_HIGHLIGHTED 0xD2
#define ITEM_1_SELECTED 0xD4
#define ITEM_2_SELECTED 0xD5
#define ITEM_3_SELECTED 0xD6

#define SERIAL_PREAMBLE_BYTE 0xFD

#define SERIAL_PREAMBLE_LENGTH 6
#define SERIAL_RN_PREAMBLE_LENGTH 7
#define SERIAL_TRADE_PREAMBLE_LENGTH 9
#define SERIAL_RNS_LENGTH 10
#define SERIAL_PATCH_LIST_PART_TERMINATOR 0xFF
#define SERIAL_NO_DATA_BYTE 0xFE

#define PKMN_MASTER 0x01
#define PKMN_SLAVE 0x02
#define PKMN_CONNECTED 0x60
#define PKMN_WAIT 0x7F

#define PKMN_ACTION 0x60

#define PKMN_TRADE_CENTRE ITEM_1_SELECTED
#define PKMN_COLOSSEUM ITEM_2_SELECTED
#define PKMN_BREAK_LINK ITEM_3_SELECTED

#define TRADE_CENTRE_WAIT 0xFD

typedef enum { NOT_CONNECTED, CONNECTED, TRADE_CENTRE, COLOSSEUM } connection_state_t;
typedef enum {
    TRADE_RESET,
    INIT,
    TRADE_RANDOM,
    SENDING_DATA,
    SENDING_PATCH_DATA,
    TRADE_PENDING,
    TRADE_CONFIRMATION,
    DONE
} trade_centre_state_t;

typedef enum {
    GAMEBOY_INITIAL,
    GAMEBOY_READY,
    GAMEBOY_WAITING,
    GAMEBOY_TRADE_READY,
    GAMEBOY_SEND,
    GAMEBOY_PENDING,
    GAMEBOY_TRADING
} render_gameboy_state_t;

/* Anonymous struct */
struct trade_ctx {
    trade_centre_state_t trade_centre_state;
    connection_state_t connection_state;
    FuriTimer* draw_timer;
    View* view;
    uint8_t in_data;
    uint8_t out_data;
    uint8_t shift;
    TradeBlock* trade_block;
    TradeBlock* input_block;
    const PokemonTable* pokemon_table;
    struct patch_list* patch_list;

    /* XXX: Lets add a variable back here to track trade state, to allow the main menu
     * to show a second option of _continue_ trade, as well as modify the default trade
     * option text to something like "restart trade/link/etc"
     */
};

/* These are the needed variables for the draw callback */
/* Technically, I think the "right" way to do this would be
 * to keep these vars in the Trade struct and copy them in to
 * the model when they may have changed. In the interest of
 * saving space they are separated. Though it may make sense
 * later down the line to keep this as a copy.
 */
struct trade_model {
    render_gameboy_state_t gameboy_status;
    bool trading;
    bool connected;
    uint8_t curr_pokemon;
    const PokemonTable* pokemon_table;
};

void pokemon_plist_recreate_callback(void* context, uint32_t arg) {
    furi_assert(context);
    UNUSED(arg);
    struct trade_ctx* trade = context;

    plist_create(&(trade->patch_list), trade->trade_block);
}

void screen_gameboy_connect(Canvas* const canvas) {
    furi_assert(canvas);

    canvas_draw_frame(canvas, 0, 0, 128, 64);
    canvas_draw_icon(canvas, 1, 21, &I_Connect_me_62x31);
    canvas_draw_icon(canvas, 0, 53, &I_Background_128x11);
    canvas_draw_icon(canvas, 80, 0, &I_game_boy);
    canvas_draw_icon(canvas, 8, 2, &I_Space_65x18);
    canvas_draw_str(canvas, 18, 13, "Connect GB");
}

void screen_gameboy_connected(Canvas* const canvas) {
    furi_assert(canvas);

    canvas_draw_frame(canvas, 0, 0, 128, 64);
    canvas_draw_icon(canvas, 1, 21, &I_Connected_62x31);
    canvas_draw_icon(canvas, 0, 53, &I_Background_128x11);
    canvas_draw_icon(canvas, 80, 0, &I_game_boy);
    canvas_draw_icon(canvas, 8, 2, &I_Space_65x18);
    canvas_draw_str(canvas, 18, 13, "Connected!");
}

static void trade_draw_callback(Canvas* canvas, void* view_model) {
    furi_assert(view_model);
    const char* gameboy_status_text = NULL;
    struct trade_model* model = view_model;
    uint8_t curr_pokemon = model->curr_pokemon;
    int time_in_seconds;

    time_in_seconds = (int)DWT->CYCCNT / (72000000.0f / 4); //  250ms

    canvas_clear(canvas);
    if(!model->trading) {
        if(!model->connected) {
            furi_hal_light_set(LightGreen, 0x00);
            furi_hal_light_set(LightBlue, 0x00);
            furi_hal_light_set(LightRed, 0xff);
            screen_gameboy_connect(canvas);
        } else {
            furi_hal_light_set(LightGreen, 0xff);
            furi_hal_light_set(LightBlue, 0x00);
            furi_hal_light_set(LightRed, 0x00);
            screen_gameboy_connected(canvas);
        }
    } else {
        switch(model->gameboy_status) {
        case GAMEBOY_TRADING:
            furi_hal_light_set(LightGreen, 0x00);
            furi_hal_light_set(LightRed, 0x00);
            if(time_in_seconds % 2 == 1) {
                furi_hal_light_set(LightBlue, 0xff);
                canvas_draw_icon(canvas, 0, 0, &I_gb_step_1);
            } else {
                furi_hal_light_set(LightBlue, 0x00);
                canvas_draw_icon(canvas, 0, 0, &I_gb_step_2);
            }
            break;
        default:
            /* Every other state, draw the pokemon we're planning to trade */
            canvas_draw_icon(canvas, 38, 11, model->pokemon_table[curr_pokemon].icon);
            break;
        }
        canvas_draw_icon(canvas, 0, 53, &I_Background_128x11);

        canvas_draw_frame(canvas, 0, 0, 128, 64);
        canvas_draw_icon(canvas, 24, 0, &I_Space_80x18);

        switch(model->gameboy_status) {
        case GAMEBOY_READY:
            gameboy_status_text = "READY";
            break;
        case GAMEBOY_WAITING:
            gameboy_status_text = "WAITING";
            break;
        case GAMEBOY_TRADE_READY:
            gameboy_status_text = "READY";
            break;
        case GAMEBOY_SEND:
            gameboy_status_text = "DEAL!";
            break;
        case GAMEBOY_PENDING:
            gameboy_status_text = "PENDING...";
            break;
        case GAMEBOY_TRADING:
            gameboy_status_text = "TRADING...";
            break;
        default:
            gameboy_status_text = "INITIAL";
            break;
        }

        canvas_draw_str(canvas, 48, 12, gameboy_status_text);

        canvas_draw_icon(canvas, 27, 1, &I_red_16x15);
    }
}

/* Get the response byte from the link partner, updating the connection
 * state if needed.
 *
 * PKMN_BLANK is an agreement between the two devices that they have
 * determined their roles
 */
static uint8_t getConnectResponse(uint8_t in, struct trade_ctx* trade) {
    furi_assert(trade);
    uint8_t ret;

    switch(in) {
    case PKMN_CONNECTED:
        trade->connection_state = CONNECTED;
        ret = PKMN_CONNECTED;
        break;
    case PKMN_MASTER:
        ret = PKMN_SLAVE;
        break;
    case PKMN_BLANK:
        ret = PKMN_BLANK;
        break;
    default:
        trade->connection_state = NOT_CONNECTED;
        ret = PKMN_BREAK_LINK;
        break;
    }

    return ret;
}

/* Receive what the pokemon game is requesting and move to that mode.
 *
 * This reads bytes sent by the gameboy and responds. The only things
 * we care about are when menu items are actually selected. The protocol
 * seems to send data both when one of the link menu items is highlighted
 * and when one of them is selected.
 *
 * If somehow we get a leader/master byte received, then go back to the
 * NOT_CONNECTED state. For the leader/master byte likely means that
 * the linked gameboy is still trying to negotiate roles and we need to
 * respond with a follower/slave byte.
 *
 * Note that, we can probably eventually drop colosseum/battle connections,
 * though it may be an interesting exercise in better understanding how the
 * "random" seeding is done between the units. As noted here:
 * http://www.adanscotney.com/2014/01/spoofing-pokemon-trades-with-stellaris.html
 * it is presumed these bytes are to sync the RNG seed between the units to
 * not need arbitration on various die rolls.
 *
 * This is where we loop if we end up in the colosseum
 */
static uint8_t getMenuResponse(uint8_t in, struct trade_ctx* trade) {
    furi_assert(trade);

    uint8_t response = PKMN_BLANK;
    /* XXX: Shouldn't this return a valid response for each option? 
     * e.g. if the gameboy selects trade center, should we also send trade center? 
     * or is the 0x00 an Agreement byte? I wonder if the leader/master is the
     * only one allowed to make the selection, and if the follower/slave selects a different
     * option it just instead returns BREAK_LINK? */

    switch(in) {
    case PKMN_CONNECTED:
        response = PKMN_CONNECTED;
        break;
    case PKMN_TRADE_CENTRE:
        trade->connection_state = TRADE_CENTRE;
        with_view_model(
            trade->view,
            struct trade_model * model,
            {
                model->gameboy_status = GAMEBOY_READY;
                model->trading = true;
            },
            false);
        break;
    case PKMN_COLOSSEUM:
        /* XXX: Ignore this */
        trade->connection_state = COLOSSEUM;
        with_view_model(
            trade->view,
            struct trade_model * model,
            {
                model->gameboy_status = GAMEBOY_READY;
                model->trading = true;
            },
            false);
        break;
    case PKMN_BREAK_LINK:
    case PKMN_MASTER:
        trade->connection_state = NOT_CONNECTED;
        response = PKMN_BREAK_LINK;
        break;
    default:
        response = in;
        break;
    }

    return response;
}

static uint8_t getTradeCentreResponse(uint8_t in, struct trade_ctx* trade) {
    furi_assert(trade);

    uint8_t* trade_block_flat = (uint8_t*)trade->trade_block;
    uint8_t* input_block_flat = (uint8_t*)trade->input_block;
    uint8_t* input_party_flat = (uint8_t*)trade->input_block->party;
    static int counter; // Should be able to be made static in used function
        // May need to make another state PRE-init or something to reset this on re-entry?
    struct trade_model* model = NULL;
    static uint8_t in_pokemon_num;
    uint8_t send = in;
    static bool patch_pt_2;

    /* TODO: Figure out how we should respond to a no_data_byte and/or how to send one
     * and what response to expect.
     */

    /* Since this is a fairly long function, it doesn't call any other functions,
     * the view model isn't locked, and we're in an interrupt context, lets just
     * mape the view model to a local var and commit it back when we're done.
     */
    model = view_get_model(trade->view);

    /* There is a handful of commucations that happen once the gameboy
     * clicks on the table. For all of them, the Flipper can just mirror back
     * the byte the gameboy sends. We can spin in this forever until we see 10x
     * SERIAL_PREAMBLE_BYTEs. Once we receive those, the counters are synched,
     * and every byte after that can be easily counted for the actual transferr
     * of Pokemon data.
     */
    switch(trade->trade_centre_state) {
    case TRADE_RESET:
        /* Reset counters and other static variables */
        counter = 0;
        patch_pt_2 = false;
        trade->trade_centre_state = INIT;
        break;

    /* This state runs through the end of the random preamble */
    case INIT:
        if(in == SERIAL_PREAMBLE_BYTE) {
            counter++;
        }
        if(counter == SERIAL_RNS_LENGTH) {
            trade->trade_centre_state = TRADE_RANDOM;
            counter = 0;
        }
        break;

    /* Once we start getting PKMN_BLANKs, we get them until we get 10x
     * SERIAL_PREAMBLE_BYTE, and then 10 random numbers. The 10 random
     * numbers are for synchrinizing the PRNG between the two systems,
     * we do not use these numbers at this time.
     */
    /* The leader/master sends 10 random bytes. This is to synchronize the RNG
     * between the connected systems. I don't think this is really needed for
     * trade, only for battles so that both sides resolve chance events exactly
     * the same way.
     *
     * Note that every random number returned is forced to be less than FD
     */
    /* This also waits through the end of the trade block preamble */
    case TRADE_RANDOM:
        counter++;
        if(counter == (SERIAL_RNS_LENGTH + SERIAL_TRADE_PREAMBLE_LENGTH)) {
            trade->trade_centre_state = SENDING_DATA;
            counter = 0;
        }
        break;

    /* This is where we get the data from gameboy that is their trade struct */
    case SENDING_DATA:
        input_block_flat[counter] = in;
        send = trade_block_flat[counter];
        counter++;

        /* XXX: TODO: right now, tradeblock is padded with 3 extra 0x00s.
	 * These are normally transmitted but we can do away with the padding
	 * by adding another state between this and the next state to wait for
	 * the padding to finish.
	 */
        if(counter == sizeof(TradeBlock)) trade->trade_centre_state = SENDING_PATCH_DATA;

        break;

    /* XXX: This seems to end with the gameboy sending DF FE 15? */

    /* A couple of FD bytes are sent, looks like 6, which means I don't think we can use count of FD bytes to see what mode we're in */
    /* XXX: THIS IS TESTED AND WORKING AS OF 20231025!
     * That means we for sure start this state and leave ths state at the right
     * parts of communication */
    case SENDING_PATCH_DATA:
        if(in == SERIAL_PREAMBLE_BYTE) {
            counter = 0;
            send = SERIAL_PREAMBLE_BYTE;
        } else {
            /* This magic number is basically the header length, 10, minus
	     * the 3x 0xFD that we should be transmitting as part of the path
	     * list header.
	     */
            if(counter > 6) {
                send = plist_index_get(trade->patch_list, (counter - 7));
            }

            /* Patch received data */
            /* This relies on the data sent only ever sending 0x00 after
             * part 2 of the patch list has been terminated. This is the
             * case in official Gen I code at this time.
             */
            switch(in) {
            case PKMN_BLANK:
                break;
            case SERIAL_PATCH_LIST_PART_TERMINATOR:
                patch_pt_2 = true;
                break;
            default: // Any nonzero value will cause a patch
                if(!patch_pt_2) {
                    /* Pt 1 is 0x00 - 0xFB */
                    input_party_flat[in - 1] = SERIAL_NO_DATA_BYTE;
                } else {
                    /* Pt 2 is 0xFC - 0x107 */
                    input_party_flat[0xFC + in - 1] = SERIAL_NO_DATA_BYTE;
                }
                break;
            }

            counter++;
            /* This is actually 200 bytes, but that includes the 3x 0xFD that we
	     * sent without counting.
	     */
            if(counter == 196) {
                trade->trade_centre_state = TRADE_PENDING;
            }
        }
        break;

    case TRADE_PENDING:
        /* TODO: What are these states */
        /* 0x6f is "close session?" */
        if(in == 0x6F) {
            trade->trade_centre_state = INIT;
            send = 0x6F;
            model->gameboy_status = GAMEBOY_TRADE_READY;
            /* 0x6? says what pokemon the gameboy is sending us */
        } else if((in & 0x60) == 0x60) {
            in_pokemon_num = in;
            send = 0x60; // first pokemon
            model->gameboy_status = GAMEBOY_SEND;
            /* I think this is a confirmation of what is being traded, likely from the dialog of:
	     * so and so will be traded for so and so, is that ok?
	     */
        } else if(in == 0x00) {
            if(in_pokemon_num != 0) {
                send = 0;
                trade->trade_centre_state = TRADE_CONFIRMATION;
                in_pokemon_num &= 0x0F;
            }
        }
        /* XXX: Test to make sure saying no at is this okay does the right thing */
        /* XXX: It does not */
        break;

    /* XXX: The actual trade uses 0x62 a bunch? Is that the OKAY? Is 0x61 a NAK? Other docs show 0x6F? */
    /* XXX: The pending text never really shows up. Maybe have a "DEAL?" and "DEAL!" state instead?
     * Actually, I think that goes by too quick, I think once you accept it goes straight in to trade.
     * So maybe the PENNDING state is actually useless? */
    case TRADE_CONFIRMATION:
        if(in == 0x61) {
            trade->trade_centre_state = TRADE_PENDING;
            model->gameboy_status = GAMEBOY_PENDING;
        } else if((in & 0x60) == 0x60) {
            trade->trade_centre_state = DONE;
        }
        break;

    case DONE:
        if(in == PKMN_BLANK) {
            send = 0;
            trade->trade_centre_state = TRADE_RESET;
            /* XXX: I think I want to change this? */
            model->gameboy_status = GAMEBOY_TRADING;

            counter = 0;

            /* Copy the traded-in pokemon's main data to our struct */
            trade->trade_block->party_members[0] =
                trade->input_block->party_members[in_pokemon_num];
            memcpy(
                &(trade->trade_block->party[0]),
                &(trade->input_block->party[in_pokemon_num]),
                sizeof(struct pokemon_structure));
            memcpy(
                &(trade->trade_block->nickname[0]),
                &(trade->input_block->nickname[in_pokemon_num]),
                sizeof(struct name));
            memcpy(
                &(trade->trade_block->ot_name[0]),
                &(trade->input_block->ot_name[in_pokemon_num]),
                sizeof(struct name));
            model->curr_pokemon = pokemon_table_get_num_from_index(
                trade->pokemon_table, trade->trade_block->party_members[0]);

            /* Schedule a callback outside of ISR context to rebuild the patch list */
            furi_timer_pending_callback(pokemon_plist_recreate_callback, trade, 0);
        }
        break;

    default:
        // Do Nothing
        break;
    }

    view_commit_model(trade->view, false);

    return send;
}

void transferBit(void* context) {
    furi_assert(context);

    struct trade_ctx* trade = (struct trade_ctx*)context;
    bool connected;

    /* We use with_view_model since the functions called here could potentially
     * also need to use the model resources. Right now this is not an issue, but
     * if this were to ever end up having a lock, it could cause access issues.
     */
    with_view_model(
        trade->view, struct trade_model * model, { connected = model->connected; }, false);

    /* Shift data in every clock */
    trade->in_data <<= 1;
    trade->in_data |= !!furi_hal_gpio_read(&GAME_BOY_SI);
    trade->shift++;

    /* Once a byte of data has been shifted in, process it */
    if(trade->shift > 7) {
        trade->shift = 0;
        switch(trade->connection_state) {
        case NOT_CONNECTED:
            connected = false;
            trade->out_data = getConnectResponse(trade->in_data, trade);
            break;
        case CONNECTED:
            connected = true;
            trade->out_data = getMenuResponse(trade->in_data, trade);
            break;
        case TRADE_CENTRE:
            trade->out_data = getTradeCentreResponse(trade->in_data, trade);
            break;
            /* If we end up in the colosseum, then just repeat data back */
            /* Do we need a way to close the connection? Would that be useful? */
        default:
            trade->out_data = trade->in_data;
            break;
        }

        trade->in_data = 0; // TODO: I don't think this is necessary?
    }

    with_view_model(
        trade->view, struct trade_model * model, { model->connected = connected; }, false);
}

void input_clk_gameboy(void* context) {
    furi_assert(context);

    struct trade_ctx* trade = (struct trade_ctx*)context;
    static uint32_t time;
    /* Clocks idle between bytes is nominally 430 us long for burst data,
     * 15 ms for idle polling (e.g. waiting for menu selection), some oddball
     * 2 ms gaps that appears between one 0xFE byte from the gameboy every trade;
     * clock period is nominally 122 us.
     * Therefore, if we havn't seen a clock in 500 us, reset our bit counter.
     * Note that, this should never actually be a concern, but it is an additional
     * safeguard against desyncing.
     */
    const uint32_t time_ticks = furi_hal_cortex_instructions_per_microsecond() * 500;

    if(furi_hal_gpio_read(&GAME_BOY_CLK)) {
        if((DWT->CYCCNT - time) > time_ticks) {
            //  IDLE & Reset
            trade->in_data = 0;
            trade->shift = 0;
        }
        transferBit(trade);
        time = DWT->CYCCNT;
    } else {
        /* On the falling edge of each clock, set up the next bit */
        furi_hal_gpio_write(&GAME_BOY_SO, trade->out_data & 0x80 ? true : false);
        trade->out_data <<= 1;
    }
}

void trade_draw_timer_callback(void* context) {
    furi_assert(context);

    struct trade_ctx* trade = (struct trade_ctx*)context;

    with_view_model(
        trade->view, struct trade_model * model, { UNUSED(model); }, true);
}

void trade_enter_callback(void* context) {
    furi_assert(context);
    struct trade_ctx* trade = (struct trade_ctx*)context;

    /* Re-init variables */
    with_view_model(
        trade->view,
        struct trade_model * model,
        {
            model->trading = false;
            model->connected = false;
            model->gameboy_status = GAMEBOY_INITIAL;
            model->pokemon_table = trade->pokemon_table;
            model->curr_pokemon = pokemon_table_get_num_from_index(
                trade->pokemon_table, trade->trade_block->party_members[0]);
        },
        true);
    trade->connection_state = NOT_CONNECTED;
    trade->trade_centre_state = INIT;

    trade->in_data = 0;
    trade->out_data = 0;
    trade->shift = 0;

    trade->draw_timer = furi_timer_alloc(trade_draw_timer_callback, FuriTimerTypePeriodic, trade);
    /* Every 100 ms, trigger a draw update */
    furi_timer_start(trade->draw_timer, furi_ms_to_ticks(100));

    /* XXX: Figure out "proper" GPIO setup/use/teardown */
    // B3 (Pin6) / SO (2)
    furi_hal_gpio_write(&GAME_BOY_SO, false);
    furi_hal_gpio_init(&GAME_BOY_SO, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    // B2 (Pin5) / SI (3)
    furi_hal_gpio_write(&GAME_BOY_SI, false);
    furi_hal_gpio_init(&GAME_BOY_SI, GpioModeInput, GpioPullUp, GpioSpeedVeryHigh);
    // // C3 (Pin7) / CLK (5)
    furi_hal_gpio_init(&GAME_BOY_CLK, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_remove_int_callback(&GAME_BOY_CLK);

    furi_hal_gpio_add_int_callback(&GAME_BOY_CLK, input_clk_gameboy, trade);

    /* Create a trade patch list from the current trade block */
    plist_create(&(trade->patch_list), trade->trade_block);
}

void disconnect_pin(const GpioPin* pin) {
    /* Existing projects seem to set the pin back to analog mode upon exit */
    furi_hal_gpio_init_simple(pin, GpioModeAnalog);
}

void trade_exit_callback(void* context) {
    furi_assert(context);

    struct trade_ctx* trade = (struct trade_ctx*)context;

    furi_hal_light_set(LightGreen, 0x00);
    furi_hal_light_set(LightBlue, 0x00);
    furi_hal_light_set(LightRed, 0x00);

    /* Stop the timer, and deallocate it as the enter callback allocates it on entry */
    furi_timer_free(trade->draw_timer);

    plist_free(trade->patch_list);
}

void* trade_alloc(TradeBlock* trade_block, const PokemonTable* table, View* view) {
    furi_assert(trade_block);
    furi_assert(view);

    struct trade_ctx* trade = malloc(sizeof(struct trade_ctx));

    trade->view = view;
    trade->trade_block = trade_block;
    trade->input_block = malloc(sizeof(TradeBlock));
    trade->pokemon_table = table;
    trade->patch_list = NULL;

    view_set_context(trade->view, trade);
    view_allocate_model(trade->view, ViewModelTypeLockFree, sizeof(struct trade_model));

    view_set_draw_callback(trade->view, trade_draw_callback);
    view_set_enter_callback(trade->view, trade_enter_callback);
    view_set_exit_callback(trade->view, trade_exit_callback);

    return trade;
}

void trade_free(void* trade_ctx) {
    furi_assert(trade_ctx);

    struct trade_ctx* trade = (struct trade_ctx*)trade_ctx;

    // Free resources
    furi_hal_gpio_remove_int_callback(&GAME_BOY_CLK);

    disconnect_pin(&GAME_BOY_CLK);

    view_free_model(trade->view);
    view_free(trade->view);
    free(trade->input_block);
    free(trade);
}
