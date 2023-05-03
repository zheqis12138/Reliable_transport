#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"
#include "buffer.h"


struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    int maxwnd;
    int timeout;

    // sender
    int snd_una;
    int snd_nxt;
    int snd_wnd;
    buffer_t* send_buffer;
    int snd_eof;

    // receiver
    int rcv_nxt;
    buffer_t* rec_buffer;
    int rcv_eof;
};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
// set up the state for a connection and return a point to it.
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
            const struct config_common *cc)
{
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
        rel_list->prev = &r->next;
    rel_list = r;

    r->maxwnd = cc->window + 1;
    r->timeout = cc->timeout;

    // sender
    r->snd_una = 1;
    r->snd_nxt = 1;
    r->snd_wnd = 1;
    r->snd_eof = 0;
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    // receiver
    r-> rcv_nxt = 1;
    r->rcv_eof = 0;
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;
    return r;
}

void
rel_destroy (rel_t *r)
{
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy (r->c);
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    free(r);
}

// n is the expected length of pkt
/* The packet is received already. Implement function for receiving packets
like error checking, updating states, sending ack... */
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    uint32_t ackno = (uint32_t) ntohl(pkt->ackno);
    uint32_t seqno = (uint32_t) ntohl(pkt->seqno);
    uint16_t pkt_cksum = pkt->cksum;
    pkt->cksum = 0;
    // drop packet if its corrupted
    if(pkt_cksum != cksum((void*) pkt, n)){
        return;
    }

    // if the packet is ACK
    if(n==8){
        if(ackno>r->snd_una){
            r->snd_una = ackno;
            buffer_remove(r->send_buffer, ackno);
        }
        r->snd_wnd = r->snd_nxt - r->snd_una;

        // of both sender and receiver reach end of file states, transfer finished and destroy the link.
        if((r->rcv_eof == 1) && (r->snd_eof == 1) && (r->snd_wnd) == 0){
            rel_destroy(r);
        }
        else if(r->snd_wnd < r->maxwnd){
            rel_read(r);
        }
        return;
    }

        // if its data packet ot eof packet
    else {

        // if packet already finish data receiving, ignore coming data packets;
        if(r->rcv_eof == 1){
            return;
        }

        // packet already in buffer, send ack but don't buffer
        if(seqno < r->rcv_nxt){
            // send back ack
            struct ack_packet *ackpkt;
            ackpkt = xmalloc(sizeof(struct ack_packet));
            ackpkt->len = htons(8);
            ackpkt->ackno = htonl(r->rcv_nxt);
            ackpkt->cksum = 0;
            ackpkt->cksum = cksum(ackpkt, 8);
            conn_sendpkt(r->c, (packet_t *) ackpkt, 8);
            return;
        }

        // only proceed packet if it's inside the receiver window
        if(seqno < r->rcv_nxt + r->maxwnd){
            // buffer the packet if it's not in buffer yet
            if(buffer_contains(r->rec_buffer, seqno)==0){
                struct timeval now;
                gettimeofday(&now, NULL);
                buffer_insert(r->rec_buffer, pkt, (now.tv_sec * 1000 + now.tv_sec / 1000));
            }

            // if the receiving packet is at the right sequence of the packet stream
            if (seqno == r->rcv_nxt){
                buffer_node_t *current = buffer_get_first(r->rec_buffer);
                r->rcv_nxt++;
                while (current->next != NULL) {
                    if(!buffer_contains(r->rec_buffer, ntohl(current->packet.seqno)+1)){
                        break;
                    }
                    r->rcv_nxt++;
                    current = current->next;
                }
                rel_output(r);
            }
            // send back ack packet
            struct ack_packet *ackpkt;
            ackpkt = xmalloc(sizeof(struct ack_packet));
            ackpkt->len = htons(8);
            ackpkt->ackno = htonl(r->rcv_nxt);
            ackpkt->cksum = 0;
            ackpkt->cksum = cksum(ackpkt, 8);
            conn_sendpkt(r->c, (packet_t *) ackpkt, 8);

        }
    }
}

void
rel_read (rel_t *s)
{
    // if send window is less than the max window, read input from stdin, make packet, buffer and send the packet.
    // finally change the states of the connection.

    while(s->snd_wnd < s->maxwnd){
        // construct packet
        packet_t *pkt;
        pkt = xmalloc(sizeof(packet_t));
        int nrBytes = conn_input(s->c, pkt->data, 500);
        // if no available input, do nothing
        if(nrBytes == 0){
            free(pkt);
            return;
        }
        // else if end of file, send eof packet
        if(nrBytes == -1){
            nrBytes = 0;
            s->snd_eof = 1;
        }
        pkt->len = htons(nrBytes + 12);
        pkt->seqno = htonl(s->snd_nxt);
        pkt->ackno = s->rcv_nxt;
        pkt->cksum = 0;
        pkt->cksum = cksum(pkt, nrBytes+12);

        // buffer and send packet
        struct timeval now;
        gettimeofday(&now, NULL);
        long curr_time = now.tv_sec * 1000 + now.tv_sec / 1000;
        buffer_insert(s->send_buffer, pkt, curr_time);
        conn_sendpkt(s->c, pkt, nrBytes+12);
        // change status
        s->snd_nxt++;
        s->snd_wnd = s->snd_nxt - s->snd_una;
        free(pkt);
    }
}

/* Called when convenient. Used to take date that is received from the network, then output the data
to the console */
void
rel_output (rel_t *r)
{
    buffer_node_t *current = buffer_get_first(r->rec_buffer);
    while (current != NULL) {

        /* if the eof packet is on the right place, it means the job of the receiver has finished, so output eof
         * to stdout and set rev_eof to 1 */
        if((ntohl(current->packet.seqno) < r->rcv_nxt) && ntohs(current->packet.len) == 12){
            conn_output(r->c, NULL, 0);
            r->rcv_eof = 1;
            return;
        }

        // if the packet is on the right place, output it to stdout and remove it from the output buffer.
        if (ntohl(current->packet.seqno) < r->rcv_nxt) {
            conn_output(r->c, current->packet.data, ntohs(current->packet.len)-12);
            buffer_remove_first(r->rec_buffer);
        }
        current = current->next;
    }
}

void
rel_timer ()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired

    rel_t *current = rel_list;
    while (current != NULL) {
        buffer_node_t *currentpkt = buffer_get_first(rel_list->send_buffer);
        while(currentpkt != NULL){
            struct timeval now;
            gettimeofday(&now, NULL);
            long curr_time = now.tv_sec * 1000 + now.tv_sec / 1000;
            long timediff = curr_time - currentpkt->last_retransmit;
            if(timediff >= rel_list->timeout){
                conn_sendpkt(rel_list->c, &currentpkt->packet, ntohs(currentpkt->packet.len));
                currentpkt->last_retransmit = curr_time;
            }
            currentpkt = currentpkt->next;
        }
        current = rel_list->next;
    }
}
