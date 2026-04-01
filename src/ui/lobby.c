#include "chess_app/ui_lobby.h"
#include "chess_app/ui_fonts.h"
#include "chess_app/network_peer.h"

void chess_lobby_render(
    SDL_Renderer *renderer,
    int width,
    int height,
    const ChessLobbyState *lobby,
    TTF_Font *font)
{
    const int peer_row_height = 36;
    const int peer_row_gap = 6;
    const int margin = 10;
    const int peer_item_width = 400;
    const int peer_item_x = (width - peer_item_width) / 2;
    int i;
    int y = margin + 20;

    if (!renderer || !lobby || !font) {
        return;
    }

    /* Title */
    {
        SDL_Texture *title_tex = make_text_texture(
            renderer, font, "Discover players - Click to challenge", (SDL_Color){238, 238, 210, 255});
        if (title_tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(title_tex, &tex_w, &tex_h);
            dst.x = (float)(width - (int)tex_w) / 2.0f;
            dst.y = (float)margin;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(renderer, title_tex, NULL, &dst);
            SDL_DestroyTexture(title_tex);
        }
    }

    y = margin + 50;

    /* Render peer list */
    for (i = 0; i < lobby->discovered_peer_count; ++i) {
        const ChessDiscoveredPeerState *peer_state = &lobby->discovered_peers[i];
        SDL_Color bg_color;
        const SDL_Color text_color = (SDL_Color){238, 238, 210, 255};

        if (peer_state->challenge_state == CHESS_CHALLENGE_OUTGOING_PENDING ||
            peer_state->challenge_state == CHESS_CHALLENGE_MATCHED) {
            bg_color = (SDL_Color){100, 150, 200, 255};
        } else if (peer_state->challenge_state == CHESS_CHALLENGE_INCOMING_PENDING) {
            bg_color = (SDL_Color){150, 180, 100, 255};
        } else if (peer_state->challenge_state == CHESS_CHALLENGE_CONNECT_FAILED) {
            bg_color = (SDL_Color){180, 60, 60, 255};
        } else if (i == lobby->hovered_peer_idx) {
            bg_color = (SDL_Color){85, 85, 85, 255};
        } else {
            bg_color = (SDL_Color){60, 60, 60, 255};
        }
        SDL_FRect peer_rect = {
            (float)peer_item_x,
            (float)y,
            (float)peer_item_width,
            (float)peer_row_height
        };

        /* Draw background rectangle */
        SDL_SetRenderDrawColor(
            renderer,
            bg_color.r,
            bg_color.g,
            bg_color.b,
            bg_color.a
        );
        SDL_RenderFillRect(renderer, &peer_rect);

        /* Draw border */
        SDL_SetRenderDrawColor(renderer, 180, 180, 180, 255);
        SDL_RenderRect(renderer, &peer_rect);

        /* Draw peer label and challenge state */
        {
            const char *challenge_icon = lobby_state_suffix(peer_state->challenge_state);
            SDL_Texture *name_tex = NULL;
            SDL_Texture *host_tex = NULL;
            SDL_Texture *status_tex = NULL;
            float name_w = 0.0f;
            float name_h = 0.0f;
            float host_w = 0.0f;
            float host_h = 0.0f;
            float status_w = 0.0f;
            float status_h = 0.0f;
            float cursor_x = peer_rect.x + 15.0f;
            float text_y;
            float max_h = 0.0f;
            const SDL_Color host_color = (SDL_Color){170, 170, 170, 255};

            if (peer_state->peer.username[0] != '\0' && peer_state->peer.hostname[0] != '\0') {
                char host_label[CHESS_PEER_HOSTNAME_MAX_LEN + 2];
                SDL_snprintf(host_label, sizeof(host_label), "@%s", peer_state->peer.hostname);
                name_tex = make_text_texture(renderer, font, peer_state->peer.username, text_color);
                host_tex = make_text_texture(renderer, font, host_label, host_color);
            } else {
                char id_label[16];
                SDL_snprintf(id_label, sizeof(id_label), "%.8s...", peer_state->peer.profile_id);
                name_tex = make_text_texture(renderer, font, id_label, text_color);
            }

            status_tex = make_text_texture(renderer, font, challenge_icon, text_color);

            if (name_tex) {
                SDL_GetTextureSize(name_tex, &name_w, &name_h);
                if (name_h > max_h) {
                    max_h = name_h;
                }
            }
            if (host_tex) {
                SDL_GetTextureSize(host_tex, &host_w, &host_h);
                if (host_h > max_h) {
                    max_h = host_h;
                }
            }
            if (status_tex) {
                SDL_GetTextureSize(status_tex, &status_w, &status_h);
                if (status_h > max_h) {
                    max_h = status_h;
                }
            }
            if (max_h <= 0.0f) {
                max_h = 18.0f;
            }

            text_y = peer_rect.y + (peer_rect.h - max_h) / 2.0f;

            if (name_tex) {
                SDL_FRect dst = {cursor_x, text_y + (max_h - name_h) / 2.0f, name_w, name_h};
                /* Slightly thicken username to approximate bold emphasis. */
                SDL_RenderTexture(renderer, name_tex, NULL, &dst);
                dst.x += 1.0f;
                SDL_RenderTexture(renderer, name_tex, NULL, &dst);
                cursor_x += name_w + 2.0f;
            }

            if (host_tex) {
                SDL_FRect dst = {cursor_x, text_y + (max_h - host_h) / 2.0f, host_w, host_h};
                SDL_RenderTexture(renderer, host_tex, NULL, &dst);
                cursor_x += host_w + 10.0f;
            } else {
                cursor_x += 10.0f;
            }

            if (status_tex) {
                SDL_FRect dst = {cursor_x, text_y + (max_h - status_h) / 2.0f, status_w, status_h};
                SDL_RenderTexture(renderer, status_tex, NULL, &dst);
            }

            if (status_tex) { SDL_DestroyTexture(status_tex); }
            if (host_tex) { SDL_DestroyTexture(host_tex); }
            if (name_tex) { SDL_DestroyTexture(name_tex); }
        }

        y += peer_row_height + peer_row_gap;
    }

    /* If no peers, show waiting message */
    if (lobby->discovered_peer_count == 0) {
        SDL_Texture *waiting_tex = make_text_texture(
            renderer, font, "Scanning for opponents...", (SDL_Color){180, 180, 180, 255});
        if (waiting_tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(waiting_tex, &tex_w, &tex_h);
            dst.x = (float)(width - (int)tex_w) / 2.0f;
            dst.y = (float)(height - (int)tex_h) / 2.0f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(renderer, waiting_tex, NULL, &dst);
            SDL_DestroyTexture(waiting_tex);
        }
    }
}

int chess_lobby_find_clicked_peer(
    SDL_Window *window,
    const ChessLobbyState *lobby,
    int mouse_x,
    int mouse_y)
{
    const int peer_row_height = 36;
    const int peer_row_gap = 6;
    const int margin = 10;
    const int peer_item_width = 400;
    int width = 0;
    int height = 0;
    int peer_item_x;
    int lobby_start_y;
    int peer_idx;

    if (!window || !lobby) {
        return -1;
    }

    SDL_GetWindowSize(window, &width, &height);
    peer_item_x = (width - peer_item_width) / 2;
    lobby_start_y = margin + 50;

    for (peer_idx = 0; peer_idx < lobby->discovered_peer_count; ++peer_idx) {
        const int peer_y = lobby_start_y + peer_idx * (peer_row_height + peer_row_gap);
        if (mouse_x >= peer_item_x &&
            mouse_x < peer_item_x + peer_item_width &&
            mouse_y >= peer_y &&
            mouse_y < peer_y + peer_row_height) {
            return peer_idx;
        }
    }

    return -1;
}
