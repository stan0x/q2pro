/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sv_user.c -- server code for moving users

#include "sv_local.h"

edict_t    *sv_player;

/*
============================================================

USER STRINGCMD EXECUTION

sv_client and sv_player will be valid.
============================================================
*/

/*
================
SV_CreateBaselines

Entity baselines are used to compress the update messages
to the clients -- only the fields that differ from the
baseline will be transmitted
================
*/
static void create_baselines( void ) {
    int        i;
    edict_t    *ent;
    entity_state_t *base, **chunk;

    // clear baselines from previous level
    for( i = 0; i < SV_BASELINES_CHUNKS; i++ ) {
        base = sv_client->baselines[i];
        if( !base ) {
            continue;
        } 
        memset( base, 0, sizeof( *base ) * SV_BASELINES_PER_CHUNK );
    }

    for( i = 1; i < sv_client->pool->num_edicts; i++ ) {
        ent = EDICT_POOL( sv_client, i );

        if( ( g_features->integer & GMF_PROPERINUSE ) && !ent->inuse ) {
            continue;
        }

        if( !ES_INUSE( &ent->s ) ) {
            continue;
        }

        ent->s.number = i;

        chunk = &sv_client->baselines[i >> SV_BASELINES_SHIFT];
        if( *chunk == NULL ) {
            *chunk = SV_Mallocz( sizeof( *base ) * SV_BASELINES_PER_CHUNK );
        }

        base = *chunk + ( i & SV_BASELINES_MASK );

        *base = ent->s;
    }

}

static void write_plain_configstrings( void ) {
    int     i;
    char    *string;
    size_t  length;

    // write a packet full of data
    string = sv_client->configstrings;
    for( i = 0; i < MAX_CONFIGSTRINGS; i++, string += MAX_QPATH ) {
        if( !string[0] ) {
            continue;
        }
        length = strlen( string );
        if( length > MAX_QPATH ) {
            length = MAX_QPATH;
        }
        // check if this configstring will overflow
        if( msg_write.cursize + length + 64 > sv_client->netchan->maxpacketlen )
        {
            SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );
        }

        MSG_WriteByte( svc_configstring );
        MSG_WriteShort( i );
        MSG_WriteData( string, length );
        MSG_WriteByte( 0 );
    }

    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );
}

static void write_baseline( entity_state_t *base ) {
    msgEsFlags_t flags = MSG_ES_FORCE;

    if( LONG_SOLID_SUPPORTED( sv_client->protocol, sv_client->version ) ) {
        flags |= MSG_ES_LONGSOLID;
    }
    MSG_WriteDeltaEntity( NULL, base, flags );
}

static void write_plain_baselines( void ) {
    int i, j;
    entity_state_t *base;

    // write a packet full of data
    for( i = 0; i < SV_BASELINES_CHUNKS; i++ ) {
        base = sv_client->baselines[i];
        if( !base ) {
            continue;
        }
        for( j = 0; j < SV_BASELINES_PER_CHUNK; j++ ) {
            if( base->number ) {
                // check if this baseline will overflow
                if( msg_write.cursize + 64 > sv_client->netchan->maxpacketlen ) 
                {
                    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );
                }

                MSG_WriteByte( svc_spawnbaseline );
                write_baseline( base );
            }
            base++;
        }
    }

    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );    
}

#if USE_ZLIB

static void write_compressed_gamestate( void ) {
    sizebuf_t   *buf = &sv_client->netchan->message;
    entity_state_t  *base;
    int         i, j;
    size_t      length;
    uint8_t     *patch;
    char        *string;

    MSG_WriteByte( svc_gamestate );

    // write configstrings
    string = sv_client->configstrings;
    for( i = 0; i < MAX_CONFIGSTRINGS; i++, string += MAX_QPATH ) {
        if( !string[0] ) {
            continue;
        }
        length = strlen( string );
        if( length > MAX_QPATH ) {
            length = MAX_QPATH;
        }

        MSG_WriteShort( i );
        MSG_WriteData( string, length );
        MSG_WriteByte( 0 );
    }
    MSG_WriteShort( MAX_CONFIGSTRINGS ); // end of configstrings

    // write baselines
    for( i = 0; i < SV_BASELINES_CHUNKS; i++ ) {
        base = sv_client->baselines[i];
        if( !base ) {
            continue;
        }
        for( j = 0; j < SV_BASELINES_PER_CHUNK; j++ ) {
            if( base->number ) {
                write_baseline( base );
            }
            base++;
        }
    }
    MSG_WriteShort( 0 ); // end of baselines

    SZ_WriteByte( buf, svc_zpacket );
    patch = SZ_GetSpace( buf, 2 );
    SZ_WriteShort( buf, msg_write.cursize );

    deflateReset( &svs.z );
    svs.z.next_in = msg_write.data;
    svs.z.avail_in = ( uInt )msg_write.cursize;
    svs.z.next_out = buf->data + buf->cursize;
    svs.z.avail_out = ( uInt )( buf->maxsize - buf->cursize );
    SZ_Clear( &msg_write );

    if( deflate( &svs.z, Z_FINISH ) != Z_STREAM_END ) {
        SV_DropClient( sv_client, "deflate() failed on gamestate" );
        return;
    }

#if USE_CLIENT
    if( sv_debug_send->integer ) {
        Com_Printf( S_COLOR_BLUE"%s: comp: %lu into %lu\n",
            sv_client->name, svs.z.total_in, svs.z.total_out );
    }
#endif

    patch[0] = svs.z.total_out & 255;
    patch[1] = ( svs.z.total_out >> 8 ) & 255;
    buf->cursize += svs.z.total_out;
}

static inline int z_flush( byte *buffer ) {
    int ret;

    ret = deflate( &svs.z, Z_FINISH ); 
    if( ret != Z_STREAM_END ) {
        return ret;
    }

#if USE_CLIENT
    if( sv_debug_send->integer ) {
        Com_Printf( S_COLOR_BLUE"%s: comp: %lu into %lu\n",
            sv_client->name, svs.z.total_in, svs.z.total_out );
    }
#endif

    MSG_WriteByte( svc_zpacket );
    MSG_WriteShort( svs.z.total_out );
    MSG_WriteShort( svs.z.total_in );
    MSG_WriteData( buffer, svs.z.total_out );
    
    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );

    return ret;
}

static inline void z_reset( byte *buffer ) {
    deflateReset( &svs.z );
    svs.z.next_out = buffer;
    svs.z.avail_out = ( uInt )( sv_client->netchan->maxpacketlen - 5 );
}

static void write_compressed_configstrings( void ) {
    int     i;
    size_t  length;
    byte    buffer[MAX_PACKETLEN_WRITABLE];
    char    *string;

    z_reset( buffer );

    // write a packet full of data
    string = sv_client->configstrings;
    for( i = 0; i < MAX_CONFIGSTRINGS; i++, string += MAX_QPATH ) {
        if( !string[0] ) {
            continue;
        }
        length = strlen( string );
        if( length > MAX_QPATH ) {
            length = MAX_QPATH;
        }

        // check if this configstring will overflow
        if( svs.z.avail_out < length + 32 ) {
            // then flush compressed data
            if( z_flush( buffer ) != Z_STREAM_END ) {
                goto fail;
            }
            z_reset( buffer );
        }

        MSG_WriteByte( svc_configstring );
        MSG_WriteShort( i );
        MSG_WriteData( string, length );
        MSG_WriteByte( 0 );

        svs.z.next_in = msg_write.data;
        svs.z.avail_in = ( uInt )msg_write.cursize;
        SZ_Clear( &msg_write );

        if( deflate( &svs.z, Z_SYNC_FLUSH ) != Z_OK ) {
            goto fail;
        }
    }

    // finally flush all remaining compressed data
    if( z_flush( buffer ) != Z_STREAM_END ) {
fail:
        SV_DropClient( sv_client, "deflate() failed on configstrings" );
    }
}

#endif // USE_ZLIB

static void stuff_cmds( list_t *list ) {
    stuffcmd_t *stuff;

    LIST_FOR_EACH( stuffcmd_t, stuff, list, entry ) {
        MSG_WriteByte( svc_stufftext );
        MSG_WriteData( stuff->string, stuff->len );
        MSG_WriteByte( '\n' );
        MSG_WriteByte( 0 );
        SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );
    }
}

static const char junkchars[] =
    "!~#``&'()*`+,-./~01~2`3`4~5`67`89:~<=`>?@~ab~cd`ef~j~k~lm`no~pq`rst`uv`w``x`yz[`\\]^_`|~";

/*
================
SV_New_f

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each server load.
================
*/
void SV_New_f( void ) {
    char junk[8][16];
    int i, j, c;
    clstate_t oldstate;

    Com_DPrintf( "New() from %s\n", sv_client->name );

    oldstate = sv_client->state;
    if( sv_client->state < cs_connected ) {
        Com_DPrintf( "Going from cs_assigned to cs_connected for %s\n",
            sv_client->name );
        sv_client->state = cs_connected;
        sv_client->lastmessage = svs.realtime; // don't timeout
        time( &sv_client->connect_time );
    } else if( sv_client->state > cs_connected ) {
        Com_DPrintf( "New not valid -- already primed\n" );
        return;
    }

    if( sv_force_reconnect->string[0] && !sv_client->reconnect_var[0] &&
        !NET_IsLocalAddress( &sv_client->netchan->remote_address ) )
    {
        for( i = 0; i < 8; i++ ) {
            for( j = 0; j < 15; j++ ) {
                c = rand() | ( rand() >> 8 );
                c %= sizeof( junkchars ) - 1;
                junk[i][j] = junkchars[c];
            }
            junk[i][15] = 0;
        }

        strcpy( sv_client->reconnect_var, junk[2] );
        strcpy( sv_client->reconnect_val, junk[3] );

        SV_ClientCommand( sv_client, "set %s set\n", junk[0] );
        SV_ClientCommand( sv_client, "$%s %s connect\n", junk[0], junk[1] );
        if( rand() & 1 ) {
            SV_ClientCommand( sv_client, "$%s %s %s\n", junk[0], junk[2], junk[3] );
            SV_ClientCommand( sv_client, "$%s %s %s\n", junk[0], junk[4],
                sv_force_reconnect->string );
            SV_ClientCommand( sv_client, "$%s %s %s\n", junk[0], junk[5], junk[6] );
        } else {
            SV_ClientCommand( sv_client, "$%s %s %s\n", junk[0], junk[4],
                sv_force_reconnect->string );
            SV_ClientCommand( sv_client, "$%s %s %s\n", junk[0], junk[5], junk[6] );
            SV_ClientCommand( sv_client, "$%s %s %s\n", junk[0], junk[2], junk[3] );
        }
        SV_ClientCommand( sv_client, "$%s %s \"\"\n", junk[0], junk[0] );
        SV_ClientCommand( sv_client, "$%s $%s\n", junk[1], junk[4] );
        SV_DropClient( sv_client, NULL );
        return;
    }

    SV_ClientCommand( sv_client, "\n" );

    //
    // serverdata needs to go over for all types of servers
    // to make sure the protocol is right, and to set the gamedir
    //
    
    // create baselines for this client
    create_baselines();

    // send the serverdata
    MSG_WriteByte( svc_serverdata );
    MSG_WriteLong( sv_client->protocol );
    MSG_WriteLong( sv_client->spawncount );
    MSG_WriteByte( 0 ); // no attract loop
    MSG_WriteString( sv_client->gamedir );
    MSG_WriteShort( sv_client->slot );
    MSG_WriteString( &sv_client->configstrings[CS_NAME*MAX_QPATH] );

    // send protocol specific stuff
    switch( sv_client->protocol ) {
    case PROTOCOL_VERSION_R1Q2:
        MSG_WriteByte( 0 ); // not enhanced
        MSG_WriteShort( sv_client->version );
        MSG_WriteByte( 0 ); // no advanced deltas
        MSG_WriteByte( sv_client->pmp.strafehack );
        break;
    case PROTOCOL_VERSION_Q2PRO:
        MSG_WriteShort( sv_client->version );
        MSG_WriteByte( 2 ); // used to be GT_DEATHMATCH
        MSG_WriteByte( sv_client->pmp.strafehack );
        MSG_WriteByte( sv_client->pmp.qwmode );
        if( sv_client->version >= PROTOCOL_VERSION_Q2PRO_WATERJUMP_HACK ) {
            MSG_WriteByte( sv_client->pmp.waterhack );
        }
        break;
    default:
        break;
    }

    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );

    SV_ClientCommand( sv_client, "\n" );

    // send version string request
    if( oldstate == cs_assigned ) {
        SV_ClientCommand( sv_client, "cmd \177c version $version\n"
#if USE_AC_SERVER
            "cmd \177c actoken $actoken\n"
#endif
            );
        stuff_cmds( &sv_cmdlist_connect );
    }

    // send reconnect var request
    if( sv_force_reconnect->string[0] && !( sv_client->flags & CF_RECONNECTED ) ) {
        SV_ClientCommand( sv_client, "cmd \177c connect $%s\n",
            sv_client->reconnect_var );
    }

    Com_DPrintf( "Going from cs_connected to cs_primed for %s\n",
        sv_client->name );
    sv_client->state = cs_primed;

    memset( &sv_client->lastcmd, 0, sizeof( sv_client->lastcmd ) );

#if USE_ZLIB
    if( sv_client->flags & CF_DEFLATE ) {
        if( sv_client->netchan->type == NETCHAN_NEW ) {
            write_compressed_gamestate();
        } else {
            // FIXME: Z_SYNC_FLUSH is not efficient for baselines
            write_compressed_configstrings();
            write_plain_baselines();
        }
    } else
#endif // USE_ZLIB
    {
        write_plain_configstrings();
        write_plain_baselines();
    }

    // send next command
    SV_ClientCommand( sv_client, "precache %i\n", sv_client->spawncount );
}

/*
==================
SV_Begin_f
==================
*/
void SV_Begin_f( void ) {
    Com_DPrintf( "Begin() from %s\n", sv_client->name );

    // handle the case of a level changing while a client was connecting
    if( sv_client->state < cs_primed ) {
        Com_DPrintf( "Begin not valid -- not yet primed\n" );
        SV_New_f();
        return;
    }
    if( sv_client->state > cs_primed ) {
        Com_DPrintf( "Begin not valid -- already spawned\n" );
        return;
    }

    if( sv_force_reconnect->string[0] && !( sv_client->flags & CF_RECONNECTED ) ) {
        if( Com_IsDedicated() ) {
            Com_Printf( "%s[%s]: failed to reconnect\n", sv_client->name,
                NET_AdrToString( &sv_client->netchan->remote_address ) );
        }
        SV_DropClient( sv_client, NULL );
        return;
    }

#if USE_AC_SERVER
    if( !AC_ClientBegin( sv_client ) ) {
        return;
    }
#endif

    Com_DPrintf( "Going from cs_primed to cs_spawned for %s\n",
        sv_client->name );
    sv_client->state = cs_spawned;
    sv_client->send_delta = 0;
    sv_client->commandMsec = 1800;
    sv_client->surpressCount = 0;

    stuff_cmds( &sv_cmdlist_begin );
    
    // call the game begin function
    ge->ClientBegin( sv_player );

#if USE_AC_SERVER
    AC_ClientAnnounce( sv_client );
#endif
}

//=============================================================================

#define MAX_DOWNLOAD_CHUNK    1024

void SV_CloseDownload( client_t *client ) {
    if( client->download ) {
        Z_Free( client->download );
        client->download = NULL;
    }
    if( client->downloadname ) {
        Z_Free( client->downloadname );
        client->downloadname = NULL;
    }
    client->downloadsize = 0;
    client->downloadcount = 0;
}

/*
==================
SV_NextDownload_f
==================
*/
static void SV_NextDownload_f( void ) {
    int     r;
    int     percent;
    int     size;

    if ( !sv_client->download )
        return;

    r = sv_client->downloadsize - sv_client->downloadcount;
    if ( r > MAX_DOWNLOAD_CHUNK )
        r = MAX_DOWNLOAD_CHUNK;

    MSG_WriteByte( svc_download );
    MSG_WriteShort( r );

    sv_client->downloadcount += r;
    size = sv_client->downloadsize;
    if( !size )
        size = 1;
    percent = sv_client->downloadcount*100/size;
    MSG_WriteByte( percent );
    MSG_WriteData( sv_client->download + sv_client->downloadcount - r, r );

    if( sv_client->downloadcount == sv_client->downloadsize ) {
        SV_CloseDownload( sv_client );
    }
        
    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );

}

static void SV_DownloadFailed( void ) {
    MSG_WriteByte( svc_download );
    MSG_WriteShort( -1 );
    MSG_WriteByte( 0 );
    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );
}

/*
==================
SV_BeginDownload_f
==================
*/
static void SV_BeginDownload_f( void ) {
    char    name[MAX_QPATH];
    int     downloadsize;
    int     offset = 0;
    cvar_t  *allow;
    int     length;

    length = Q_ClearStr( name, Cmd_Argv( 1 ),  sizeof( name ) );
    Q_strlwr( name );

    if( Cmd_Argc() > 2 )
        offset = atoi( Cmd_Argv( 2 ) ); // downloaded offset

    // hacked by zoid to allow more conrol over download
    // first off, no .. or global allow check
    if( !allow_download->integer
        // check for empty paths
        || !length
        // check for illegal negative offsets
        || offset < 0
        // don't allow anything with .. path
        || strstr( name, ".." )
        // leading dots, slashes, etc are no good
        || !Q_ispath( name[0] )
        // trailing dots, slashes, etc are no good
        || !Q_ispath( name[ length - 1 ] )
        // back slashes should be never sent
        || strchr( name, '\\' )
        // colons are bad also
        || strchr( name, ':' )
        // MUST be in a subdirectory    
        || !strchr( name, '/' ) )    
    {    
        SV_DownloadFailed();
        return;
    }

    if( strncmp( name, "players/", 8 ) == 0 ) {
        allow = allow_download_players;
    } else if( strncmp( name, "models/", 7 ) == 0 ||
        strncmp( name, "sprites/", 8 ) == 0 )
    {
        allow = allow_download_models;
    } else if( strncmp( name, "sound/", 6 ) == 0 ) {
        allow = allow_download_sounds;
    } else if( strncmp( name, "maps/", 5 ) == 0 ) {
        allow = allow_download_maps;
    } else if( strncmp( name, "textures/", 9 ) == 0 ||
        strncmp( name, "env/", 4 ) == 0 )
    {
        allow = allow_download_textures;
    } else if( strncmp( name, "pics/", 5 ) == 0 ) {
        allow = allow_download_pics;
    } else {
        allow = allow_download_others;
    }

    if( !allow->integer ) {
        Com_DPrintf( "Refusing download of %s to %s\n", name, sv_client->name );
        SV_DownloadFailed();
        return;
    }

    if( sv_client->download ) {
        Com_DPrintf( "Closing existing download for %s (should not happen)\n", sv_client->name );
        SV_CloseDownload( sv_client );
    }

    downloadsize = FS_LoadFileEx( name, NULL, 0, TAG_SERVER );
    
    if( downloadsize == INVALID_LENGTH || downloadsize == 0
        // special check for maps, if it came from a pak file, don't allow
        // download  ZOID
        || ( allow == allow_download_maps
            && allow_download_maps->integer < 2
            && FS_LastFileFromPak() ) )
    {
        Com_DPrintf( "Couldn't download %s to %s\n", name, sv_client->name );
        SV_DownloadFailed();
        return;
    }

    if( offset > downloadsize ) {
        Com_DPrintf( "Refusing download, %s has wrong version of %s (%d > %d)\n",
            sv_client->name, name, offset, downloadsize );
        SV_ClientPrintf( sv_client, PRINT_HIGH, "File size differs from server.\n"
            "Please delete the corresponding .tmp file from your system.\n" );
        SV_DownloadFailed();
        return;
    }

    if( offset == downloadsize ) {
        Com_DPrintf( "Refusing download, %s already has %s (%d bytes)\n",
            sv_client->name, name, offset );
        MSG_WriteByte( svc_download );
        MSG_WriteShort( 0 );
        MSG_WriteByte( 100 );
        SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );
        return;
    }

    sv_client->downloadsize = FS_LoadFileEx( name,
        ( void ** )&sv_client->download, 0, TAG_SERVER );
    sv_client->downloadcount = offset;
    sv_client->downloadname = SV_CopyString( name );

    Com_DPrintf( "Downloading %s to %s\n", name, sv_client->name );

    SV_NextDownload_f();
}

static void SV_StopDownload_f( void ) {
    int size, percent;

    if( !sv_client->download ) {
        return;
    }

    size = sv_client->downloadsize;
    if( !size ) {
        percent = 0;
    } else {
        percent = sv_client->downloadcount*100/size;
    }

    MSG_WriteByte( svc_download );
    MSG_WriteShort( -1 );
    MSG_WriteByte( percent );
    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );

    Com_DPrintf( "Download of %s to %s stopped by user request\n",
        sv_client->downloadname, sv_client->name );
    SV_CloseDownload( sv_client );
}

//============================================================================


/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately
=================
*/
static void SV_Disconnect_f( void ) {
    if( Com_IsDedicated() && sv_client->netchan ) {
        Com_Printf( "%s[%s] disconnected\n", sv_client->name,
            NET_AdrToString( &sv_client->netchan->remote_address ) );
    }
    SV_DropClient( sv_client, NULL );
    SV_RemoveClient( sv_client ); // don't bother with zombie state
}


/*
==================
SV_ShowServerinfo_f

Dumps the serverinfo info string
==================
*/
static void SV_ShowServerinfo_f( void ) {
    char serverinfo[MAX_INFO_STRING];

    Cvar_BitInfo( serverinfo, CVAR_SERVERINFO );

    SV_BeginRedirect( RD_CLIENT );
    Info_Print( serverinfo );
    Com_EndRedirect();
}

static void SV_NoGameData_f( void ) {
    sv_client->flags ^= CF_NODATA;
}

static void SV_Lag_f( void ) {
    client_t *cl;

    if( Cmd_Argc() > 1 ) {
        SV_BeginRedirect( RD_CLIENT );
        cl = SV_EnhancedSetPlayer( Cmd_Argv( 1 ) );
        Com_EndRedirect();
        if( !cl ) {
            return;
        }
    } else {
        cl = sv_client;
    }

    SV_ClientPrintf( sv_client, PRINT_HIGH,
        "Lag stats for:       %s\n"
        "RTT (min/avg/max):   %d/%d/%d ms\n"
        "Server to client PL: %.2f%% (approx)\n"
        "Client to server PL: %.2f%%\n",
        cl->name, cl->min_ping, AVG_PING( cl ), cl->max_ping,
        PL_S2C( cl ), PL_C2S( cl ) );
}

#if USE_PACKETDUP
static void SV_PacketdupHack_f( void ) {
    int numdups = sv_client->numpackets - 1;

    if( Cmd_Argc() > 1 ) {
        numdups = atoi( Cmd_Argv( 1 ) );
        if( numdups < 0 || numdups > sv_packetdup_hack->integer ) {
            SV_ClientPrintf( sv_client, PRINT_HIGH,
                "Packetdup of %d is not allowed on this server.\n", numdups );
            return;
        }

        sv_client->numpackets = numdups + 1;
    }

    SV_ClientPrintf( sv_client, PRINT_HIGH,
        "Server is sending %d duplicate packet%s to you.\n",
        numdups, numdups == 1 ? "" : "s" );
    if( numdups > 1 ) {
        SV_ClientPrintf( sv_client, PRINT_MEDIUM, "Poor, poor server...\n" );
    }
}
#endif

static void SV_CvarResult_f( void ) {
    char *c, *v;

    c = Cmd_Argv( 1 );
    if( !strcmp( c, "version" ) ) {
        if( !sv_client->versionString ) {
            v = Cmd_RawArgsFrom( 2 );
            if( Com_IsDedicated() ) {
                Com_Printf( "%s[%s]: %s\n", sv_client->name,
                    NET_AdrToString( &sv_client->netchan->remote_address ), v );
            }
            sv_client->versionString = SV_CopyString( v );
        }
    } else if( !strcmp( c, "connect" ) ) {
        if( sv_client->reconnect_var[0] ) {
            v = Cmd_Argv( 2 );
            if( !strcmp( v, sv_client->reconnect_val ) ) {
                sv_client->flags |= CF_RECONNECTED;
            }
        }
    }
#if USE_AC_SERVER
    else if( !strcmp( c, "actoken" ) ) {
        AC_ClientToken( sv_client, Cmd_Argv( 2 ) );
    }
#endif
}

#if USE_AC_SERVER

static void SV_AC_List_f( void ) {
    SV_BeginRedirect( RD_CLIENT );
    AC_List_f();
    Com_EndRedirect();
}

static void SV_AC_Info_f( void ) {
    SV_BeginRedirect( RD_CLIENT );
    AC_Info_f();
    Com_EndRedirect();
}

#else

static void SV_AC_Null_f( void ) {
    SV_ClientPrintf( sv_client, PRINT_HIGH,
        "This server does not support anticheat.\n" );
}

#endif

static const ucmd_t ucmds[] = {
    // auto issued
    { "new", SV_New_f },
    { "begin", SV_Begin_f },
    { "baselines", NULL },
    { "configstrings", NULL },
    { "nextserver", NULL },
    { "disconnect", SV_Disconnect_f },

    // issued by hand at client consoles    
    { "info", SV_ShowServerinfo_f },

    { "download", SV_BeginDownload_f },
    { "nextdl", SV_NextDownload_f },
    { "stopdl", SV_StopDownload_f },

    { "\177c", SV_CvarResult_f },
    { "nogamedata", SV_NoGameData_f },
    { "lag", SV_Lag_f },
#if USE_PACKETDUP
    { "packetdup", SV_PacketdupHack_f },
#endif
#if USE_AC_SERVER
    { "aclist", SV_AC_List_f },
    { "acinfo", SV_AC_Info_f },
#else
    { "aclist", SV_AC_Null_f },
    { "acinfo", SV_AC_Null_f },
#endif

    { NULL, NULL }
};

static void handle_filtercmd( filtercmd_t *filter ) {
    size_t len;

    switch( filter->action ) {
    case FA_PRINT:
        MSG_WriteByte( svc_print );
        MSG_WriteByte( PRINT_HIGH );
        break;
    case FA_STUFF:
        MSG_WriteByte( svc_stufftext );
        break;
    case FA_KICK:
        SV_DropClient( sv_client, filter->comment[0] ?
            filter->comment : "issued banned command" );
        // fall through
    default:
        return;
    }

    len = strlen( filter->comment );
    MSG_WriteData( filter->comment, len );
    MSG_WriteByte( '\n' );
    MSG_WriteByte( 0 );

    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );
}

/*
==================
SV_ExecuteUserCommand
==================
*/
static void SV_ExecuteUserCommand( const char *s ) {
    const ucmd_t *u;
    filtercmd_t *filter;
    char *c;
    
    Cmd_TokenizeString( s, qfalse );
    sv_player = sv_client->edict;

    c = Cmd_Argv( 0 );
    if( !c[0] ) {
        return;
    }

    if( ( u = Com_Find( ucmds, c ) ) != NULL ) {
        if( u->func ) {
            u->func();
        }
        return;
    }
    if( sv.state < ss_game ) {
        return;
    }
    LIST_FOR_EACH( filtercmd_t, filter, &sv_filterlist, entry ) {
        if( !Q_stricmp( filter->string, c ) ) {
            handle_filtercmd( filter );
            return;
        }
    }
    ge->ClientCommand( sv_player );
}

/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
==================
SV_ClientThink
==================
*/
static inline void SV_ClientThink( usercmd_t *cmd ) {
    sv_client->commandMsec -= cmd->msec;
    sv_client->numMoves++;

    if( sv_client->commandMsec < 0 && sv_enforcetime->integer ) {
#ifdef _DEBUG
        Com_DPrintf( "commandMsec underflow from %s: %d\n",
            sv_client->name, sv_client->commandMsec );
#endif
        return;
    }

    ge->ClientThink( sv_player, cmd );
}

static inline void SV_SetLastFrame( int lastframe ) {
    unsigned sentTime;

    if( lastframe > 0 ) {
        if( lastframe > sv.framenum ) {
            return; // ignore bogus acks
        }
        if( lastframe <= sv_client->lastframe ) {
            return; // ignore duplicate acks
        }

        sentTime = sv_client->frames[lastframe & UPDATE_MASK].sentTime;
        if( sentTime <= com_eventTime ) {
            sv_client->frame_latency[lastframe & LATENCY_MASK] = com_eventTime - sentTime;
        }

        if( sv_client->state == cs_spawned ) {
            sv_client->frames_acked++;
        }
    }

    sv_client->lastframe = lastframe;
}

/*
==================
SV_OldClientExecuteMove
==================
*/
static void SV_OldClientExecuteMove( int net_drop ) {
    usercmd_t   oldest, oldcmd, newcmd;
    int         lastframe;

    if( sv_client->protocol == PROTOCOL_VERSION_DEFAULT ) {
        MSG_ReadByte();    // skip over checksum
    }
    
    lastframe = MSG_ReadLong();
    SV_SetLastFrame( lastframe );

    if( sv_client->protocol == PROTOCOL_VERSION_R1Q2 &&
        sv_client->version >= PROTOCOL_VERSION_R1Q2_UCMD ) 
    {
        MSG_ReadDeltaUsercmd_Hacked( NULL, &oldest );
        MSG_ReadDeltaUsercmd_Hacked( &oldest, &oldcmd );
        MSG_ReadDeltaUsercmd_Hacked( &oldcmd, &newcmd );
    } else {
        MSG_ReadDeltaUsercmd( NULL, &oldest );
        MSG_ReadDeltaUsercmd( &oldest, &oldcmd );
        MSG_ReadDeltaUsercmd( &oldcmd, &newcmd );
    }

    if( sv_client->state != cs_spawned ) {
        sv_client->lastframe = -1;
        return;
    }

    if( net_drop > 2 ) {
        sv_client->frameflags |= FF_CLIENTPRED;
    } 

    if( net_drop < 20 ) {
        while( net_drop > 2 ) {
            SV_ClientThink( &sv_client->lastcmd );
            net_drop--;
        }
        if( net_drop > 1 )
            SV_ClientThink( &oldest );

        if( net_drop > 0 )
            SV_ClientThink( &oldcmd );

    }
    SV_ClientThink( &newcmd );
    
    sv_client->lastcmd = newcmd;
}



/*
==================
SV_NewClientExecuteMove
==================
*/
static void SV_NewClientExecuteMove( int c, int net_drop ) {
    usercmd_t   cmds[MAX_PACKET_FRAMES][MAX_PACKET_USERCMDS];
    usercmd_t   *lastcmd, *cmd;
    int         lastframe;
    int         numCmds[MAX_PACKET_FRAMES], numDups;
    int         i, j, lightlevel;

    numDups = c >> SVCMD_BITS;
    c &= SVCMD_MASK;

    if( numDups >= MAX_PACKET_FRAMES ) {
        SV_DropClient( sv_client, "too many frames in packet" );
        return;
    }

    if( c == clc_move_nodelta ) {
        lastframe = -1;
    } else {
        lastframe = MSG_ReadLong();
    }

    SV_SetLastFrame( lastframe );

    lightlevel = MSG_ReadByte();

    // read all cmds
    lastcmd = NULL;
    for( i = 0; i <= numDups; i++ ) {
        numCmds[i] = MSG_ReadBits( 5 );
        if( numCmds[i] == -1 ) {
            SV_DropClient( sv_client, "read past end of message" );
            return;
        }
        if( numCmds[i] >= MAX_PACKET_USERCMDS ) {
            SV_DropClient( sv_client, "too many usercmds in frame" );
            return;
        }
        for( j = 0; j < numCmds[i]; j++ ) {
            if( msg_read.readcount > msg_read.cursize ) {
                SV_DropClient( sv_client, "read past end of message" );
                return;
            }
            cmd = &cmds[i][j];
            MSG_ReadDeltaUsercmd_Enhanced( lastcmd, cmd, sv_client->version );
            cmd->lightlevel = lightlevel;
            lastcmd = cmd;
        }
    }
    if( sv_client->state != cs_spawned ) {
        sv_client->lastframe = -1;
        return;
    }

    if( q_unlikely( !lastcmd ) ) {
        return; // should never happen
    }

    if( net_drop > numDups ) {
        sv_client->frameflags |= FF_CLIENTPRED;
    } 

    if( net_drop < 20 ) {
        // run lastcmd multiple times if no backups available
        while( net_drop > numDups ) {
            SV_ClientThink( &sv_client->lastcmd );
            net_drop--;
        }

        // run backup cmds, if any
        while( net_drop > 0 ) {
            i = numDups - net_drop;
            for( j = 0; j < numCmds[i]; j++ ) {
                SV_ClientThink(  &cmds[i][j] );
            }
            net_drop--;
        }

    }

    // run new cmds
    for( j = 0; j < numCmds[numDups]; j++ ) {
        SV_ClientThink( &cmds[numDups][j] );
    }
    
    sv_client->lastcmd = *lastcmd;
}

/*
===================
SV_ExecuteClientMessage

The current net_message is parsed for the given client
===================
*/
void SV_ExecuteClientMessage( client_t *client ) {
    int         c;
    qboolean    move_issued;
    int         stringCmdCount;
    int         userinfoUpdateCount;
    int         net_drop;
    size_t      len;

    sv_client = client;
    sv_player = sv_client->edict;

    // only allow one move command
    move_issued = qfalse;
    stringCmdCount = 0;
    userinfoUpdateCount = 0;

    net_drop = client->netchan->dropped;
    if( net_drop > 0 ) {
        client->frameflags |= FF_CLIENTDROP;
    }

    while( 1 ) {
        if( msg_read.readcount > msg_read.cursize ) {
            SV_DropClient( client, "read past end of message" );
            break;
        }    

        c = MSG_ReadByte();
        if( c == -1 )
            break;
        
        switch( c & SVCMD_MASK ) {
        default:
        badbyte:
            SV_DropClient( client, "unknown command byte" );
            break;
                        
        case clc_nop:
            break;

        case clc_userinfo: {
                char buffer[MAX_INFO_STRING];

                len = MSG_ReadString( buffer, sizeof( buffer ) );
                if( len >= sizeof( buffer ) ) {
                    SV_DropClient( client, "oversize userinfo" );
                    break;
                }

                // malicious users may try sending too many userinfo updates
                if( userinfoUpdateCount == MAX_PACKET_USERINFOS ) {
                    Com_DPrintf( "Too many userinfos from %s\n", client->name );
                    break;
                }

                SV_UpdateUserinfo( buffer );
                userinfoUpdateCount++;
            }
            break;

        case clc_move:
            if( move_issued ) {
                SV_DropClient( client, "multiple clc_move commands in packet" );
                break;        // someone is trying to cheat...
            }

            move_issued = qtrue;

            SV_OldClientExecuteMove( net_drop );
            break;

        case clc_stringcmd: {
                char buffer[MAX_STRING_CHARS];

                len = MSG_ReadString( buffer, sizeof( buffer ) );
                if( len >= sizeof( buffer ) ) {
                    SV_DropClient( client, "oversize stringcmd" );
                    break;
                }
                
                if( developer->integer ) {
                    Com_Printf( S_COLOR_BLUE "ClientCommand( %s ): %s\n",
                        client->name, Q_FormatString( buffer ) );
                }

                // malicious users may try using too many string commands
                if( stringCmdCount == MAX_PACKET_STRINGCMDS ) {
                    Com_DPrintf( "Too many stringcmds from %s\n", client->name );
                    break;
                }
                SV_ExecuteUserCommand( buffer );
                stringCmdCount++;
            }
            break;

        // r1q2 specific operations
        case clc_setting: {
                uint16_t idx, value;

                if( client->protocol < PROTOCOL_VERSION_R1Q2 ) {
                    goto badbyte;
                }

                idx = MSG_ReadShort();
                value = MSG_ReadShort();
                if( idx < CLS_MAX ) {
                    client->settings[idx] = value;
                }
            }
            break;


        // q2pro specific operations
        case clc_move_nodelta:
        case clc_move_batched:
            if( client->protocol != PROTOCOL_VERSION_Q2PRO ) {
                goto badbyte;
            }

            if( move_issued ) {
                SV_DropClient( client, "multiple clc_move commands in packet" );
                break; // someone is trying to cheat...
            }

            move_issued = qtrue;
            SV_NewClientExecuteMove( c, net_drop );
            break;

        case clc_userinfo_delta: {
                char key[MAX_INFO_KEY], value[MAX_INFO_VALUE];
                char buffer[MAX_INFO_STRING];
                
                if( client->protocol != PROTOCOL_VERSION_Q2PRO ) {
                    goto badbyte;
                }

                len = MSG_ReadString( key, sizeof( key ) );
                if( len >= sizeof( key ) ) {
                    SV_DropClient( client, "oversize delta key" );
                    break;
                }

                len = MSG_ReadString( value, sizeof( value ) );
                if( len >= sizeof( value ) ) {
                    SV_DropClient( client, "oversize delta value" );
                    break;
                }

                // malicious users may try sending too many userinfo updates
                if( userinfoUpdateCount == MAX_PACKET_USERINFOS ) {
                    Com_DPrintf( "Too many userinfos from %s\n", client->name );
                    break;
                }
                userinfoUpdateCount++;

                strcpy( buffer, client->userinfo );
                if( !Info_SetValueForKey( buffer, key, value ) ) {
                    SV_DropClient( client, "malformed delta userinfo" );
                    break;
                }

                SV_UpdateUserinfo( buffer );
            }
            break;
        }

        if( client->state < cs_assigned ) {
            break;    // disconnect command
        }
    }

    sv_client = NULL;
    sv_player = NULL;
}

