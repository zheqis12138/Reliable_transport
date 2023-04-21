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

// The state of a connection a
struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    /* Add your own data fields below this */
    // sender states
    int snd_una; // unacknowledged packet, next seqno the receiver is expected to get
    // snd_una = max(snd_una, ackno)
    int snd_nxt; // seqno of the next packet to send out
    // snd_wnd - sed_una
    buffer_t* send_buffer;
    // receiver states
    int rcv_nxt;// the next seqno the receiver is expected to receive.
    buffer_t* rec_buffer;
    int timer;
    int maxwnd;
    int timeout;
    int snd_eof;
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

    /* Do any other initialization you need here... */
    // sender side
    r->snd_una = 1;
    r->snd_nxt = 1;
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    // receiver side
    r->rcv_nxt = 1;
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;
    r->maxwnd = cc->window;
    r->timer = cc->timer;
    r->timeout = cc->timeout;

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

    /* Free any other allocated memory here */
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    free(r);
    // ...

}

// n is the expected length of pkt
/* The packet is received already. Implement function for reveiving packets
like error checking, updating states, sending ack... */
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    /* Your logic implementation here */
    int real_sum;
    int calculated_sum;

    // checksum
    real_sum = pkt->cksum;
    // calculate checksum value of a packet
    pkt->cksum = 0;

    print_pkt(pkt, "a", n);

    calculated_sum = cksum(pkt, n);
    if (calculated_sum != real_sum){
        return;
    }

    // client side (expected to receive ack packet)
    // if its ACK packet
    if (n == 8){
        if (pkt->ackno> r->snd_una){
            // update status (silde window)
            r->snd_una = pkt->ackno;
            buffer_remove(r->send_buffer, pkt->ackno);
            // send out further value is available
            if (r->snd_nxt-r->snd_una < r->maxwnd){
                rel_read(r);
            }
        }
    }

    // server side
    else {
        // if its end of file packet, and have written all output data, send EOF to output
        if (n == 12 && buffer_size(r->rec_buffer) == 0){
            conn_output(r->c, NULL, 0);
            r->rcv_eof = 1;
            return;
        }

        else if (n>12 && n<=512){
            // if packet already received, send ack but do not buffer;
            if (pkt->seqno < r->rcv_nxt){
                struct ack_packet *ackPacket;
                ackPacket = xmalloc(sizeof(struct ack_packet));
                ackPacket->len = 8;
                ackPacket->ackno = r->rcv_nxt;
                ackPacket->cksum = 0;
                ackPacket->cksum = cksum(ackPacket, 8);
                conn_sendpkt(r->c, (packet_t *)ackPacket, 8);
                return;
            }

            else if (pkt->seqno < r->rcv_nxt+r->maxwnd) {
                // buffer out-of-sequence packets, if receiver buffer is not full
                // add to output buffer
                if (!buffer_contains(r->rec_buffer, pkt->seqno)) {
                    // get current time
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;
                    // check the availability of receiver buffer
                    if (buffer_size(r->rec_buffer) < r->maxwnd) {
                        buffer_insert(r->rec_buffer, pkt, now_ms);
                    } else {
                        return;
                    }
                }

                // update states
                if (pkt->seqno == r->rcv_nxt) {
                    // find the highest consecutive seqno in the buffer
                    r->rcv_nxt++;
                    buffer_node_t *curr = buffer_get_first(r->rec_buffer);
                    while (curr->next != NULL){
                        if (!buffer_contains(r->rec_buffer, curr->packet.seqno+1)){
                            break;
                        }
                        r->rcv_nxt++;
                        curr = curr->next;
                    }

                    // write it to output with rel_output
                    rel_output(r);
                }

                // send acknoledge
                struct ack_packet *ackPacket;
                ackPacket = xmalloc(sizeof(struct ack_packet));
                ackPacket->len = 8;
                ackPacket->ackno = r->rcv_nxt;
                ackPacket->cksum = 0;
                ackPacket->cksum = cksum(ackPacket, 8);
                conn_sendpkt(r->c, (packet_t*)ackPacket, 8);
                return;
            }
        }

        // if packet size > 500, drop packet
        else{
            return;
        }
    }

    // If everything is finished, destroy the connection
    if (r->snd_eof == 1 && r->rcv_eof == 1 && r->snd_una == r->snd_nxt){
        rel_destroy(r);
    }
    
}

/* Is called whenever type something in the console.
In this case, read data from the console, put it in the packet, then to the
sending buffer(if buffer has enough space), then to the network(is slide window has enough space).
*/
void
rel_read (rel_t *s)
{
    /* Your logic implementation here */
    // if send window is less than the max window, read input from stdin, make packet, buffer and send the packet.
    // finally change the states of the connection.
    while (s->snd_nxt - s->snd_una < s->maxwnd ){
        // construct packet
        packet_t *pkt;
        pkt = xmalloc(sizeof(packet_t));
        int num_bytes = conn_input(s->c, pkt->data, 500);
        // if no available input, do nothing
        if (num_bytes == 0){
            free(pkt);
            return;
        }
        // else if end of file
        else if (num_bytes == -1){
            num_bytes = 0;
            s->snd_eof = 1;
        }
        pkt->len = 12 + num_bytes;
        pkt->ackno = s->snd_una;
        pkt->seqno = s->snd_nxt;
        pkt->cksum = 0;
        pkt->cksum = cksum(pkt, 12+num_bytes);
        // buffer the packet
        struct timeval now;
        gettimeofday(&now, NULL);
        long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;
        buffer_insert(s->send_buffer, pkt, now_ms);
        // send the packet
        conn_sendpkt(s->c, pkt, 12+num_bytes);
        // change statuss
        s->snd_nxt++;
        free(pkt);
    }
}

/* Called when convenient. Used to take date that is received from the network, then output the data
to the console */
void
rel_output (rel_t *r)
{
    /* Your logic implementation here */
    // if packet->data <= output buffer, output(call conn_output). else, do not output.
    // output the first packet in the receiver buffer
    buffer_node_t *curr_node = buffer_get_first(r->rec_buffer);
    buffer_node_t *next_node = curr_node->next;
    conn_output(r->c, curr_node->packet.data, curr_node->packet.len-12);
    buffer_remove_first(r->rec_buffer);
    // output the consequent packet if it is the right one
    while(next_node && next_node->packet.seqno - curr_node->packet.seqno == 1){
        curr_node = next_node;
        next_node = curr_node->next;
        conn_output(r->c, curr_node->packet.data, curr_node->packet.len-12);
        buffer_remove_first(r->rec_buffer);
    }
}

/* Called in pre-definded interval*/
void
rel_timer ()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    while (current != NULL) {
        // ...
        current = current->next;
    }
}
