#include <GL/glut.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "so_game_protocol.h"
#include "image.h"
#include "surface.h"
#include "world.h"
#include "vehicle.h"
#include "world_viewer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>  // htons()
#include <netinet/in.h> // struct sockaddr_in
#include <sys/socket.h>
#include <semaphore.h>

#include "player_list.h"
#include "common.h"
#include "network_func.h"

int id, s;
int current_players = 1;
World world;
Vehicle* vehicle; // The vehicle
PlayersList * players;

sem_t world_started_sem;

void * reciver(void * args) {

    int slen, ret;
    char buf[1000000];
    struct sockaddr_in si_other = *((struct sockaddr_in *) args);
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(si_other.sin_addr), client_ip, INET_ADDRSTRLEN);
    uint16_t client_port = ntohs(si_other.sin_port); // port number is an unsigned short

    ret = sem_wait(&world_started_sem);
    ERROR_HELPER(ret, "[RECIVER THREAD] Error sem wait");

    printf("[RECIVER THREAD] Start reciving update\n");
    while(1) {

        ret = recvfrom(s, buf, sizeof(buf), 0, &si_other, &slen);
        ERROR_HELPER(ret, "[RECIVER THREAD] Error recive");

        WorldUpdatePacket * wp = Packet_deserialize(buf, ret);
        ClientUpdate * cu = wp->updates;
        int i, n = wp->num_vehicles;

        if(n < current_players) {

            //Some player quit
            current_players = n;
            Player * p = players->first;
            while(p != NULL) {

                for(i = 0; i < n; i++) {
                    if(p->id == cu[i].id) break;
                }

                if(i == n) {

                    Vehicle * to_detach = World_getVehicle(&world, p->id);
                    World_detachVehicle(&world, to_detach);
                    Vehicle_destroy(to_detach);
                    free(to_detach);
                    printf("[RECIVER THREAD] Player ID %d quit the game\n", p->id);
                    player_list_delete(players, p->id);
                    break;
                }

                if(p != NULL) p = p->next;

            }

        }

        for(i = 0; i < n; i++) {
            Vehicle * v = World_getVehicle(&world, cu[i].id);
            if(v == 0 || v == NULL) break;
            v->x = cu[i].x;
            v->y = cu[i].y;
            v->theta = cu[i].theta;


        }

        World_update(&world);
    }
}

void * new_player_listener(void * args) {

    int socket_desc = *((int *) args);
    int recieved_bytes, ret;
    char buf[1000000];
    //Listening for new players
    ret = sem_wait(&world_started_sem);
    ERROR_HELPER(ret, "[NEW PLAYER THREAD] Error sem wait");

    while(1) {

        recv_packet_TCP(socket_desc, buf);
        ImagePacket * imagePacket = Packet_deserialize(buf, ((PacketHeader *) buf)->size);
        Vehicle * newvehicle=(Vehicle*) malloc(sizeof(Vehicle));
        Vehicle_init(newvehicle, &world, imagePacket->id, imagePacket->image);
        World_addVehicle(&world, newvehicle);
        current_players += 1;
        player_list_insert(players, imagePacket->id, NULL);

        printf("[NEW PLAYER THREAD] Player %d added to the world\n", imagePacket->id);

    }

}

void * update_sender(void * args) {

    char buf[1000000];
    int ret;

    struct sockaddr_in * si_other = (struct sockaddr_in *) args;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(si_other->sin_addr), client_ip, INET_ADDRSTRLEN);
    uint16_t client_port = ntohs(si_other->sin_port); // port number is an unsigned short


    ret = sem_wait(&world_started_sem);
    ERROR_HELPER(ret, "[UPDATER THREAD] Error sem wait");

    printf("[UPDATER THREAD] Start sending update\n");

    while(1) {
        PacketHeader packetHeader;
        VehicleUpdatePacket  vehicleUpdatePacket;
        packetHeader.type = VehicleUpdate;
        vehicleUpdatePacket.header = packetHeader;
        vehicleUpdatePacket.id = id;
        vehicleUpdatePacket.rotational_force = vehicle->rotational_force_update;
        vehicleUpdatePacket.translational_force = vehicle->translational_force_update;
        int buf_size = Packet_serialize(buf, &(vehicleUpdatePacket.header));
        ret = sendto(s, buf, buf_size, 0, (struct sockaddr*) si_other, sizeof(*si_other));
        ERROR_HELPER(ret, "[UPDATER THREAD] Error sending updates");

        usleep(30000);
    }


}


int main(int argc, char **argv) {
    if (argc<3) {
        printf("usage: <server_address> <player texture>\n");
        exit(-1);
    }

    printf("[MAIN] Loading texture image from %s ... ", argv[2]);
    Image* my_texture = Image_load(argv[2]);
    if (my_texture) {
        printf("[MAIN] Texture loaded\n");
    } else {
        printf("[MAIN] Fail! \n");
        exit(-1);
    }

    int my_id;

    int ret, recieved_bytes;

    // variables for handling a socket
    int socket_desc;
    struct sockaddr_in server_addr = {0}; // some fields are required to be filled with 0

    // create a socket
    socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    ERROR_HELPER(socket_desc, "[MAIN] Could not create socket");

    // set up parameters for the connection
    server_addr.sin_addr.s_addr = inet_addr(SERVER_ADDRESS);
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(SERVER_PORT); // don't forget about network byte order!

    // initiate a connection on the socket
    ret = connect(socket_desc, (struct sockaddr*) &server_addr, sizeof(struct sockaddr_in));
    ERROR_HELPER(ret, "[MAIN] Could not create connection");

    char buf[1000000];

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(server_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    uint16_t client_port = ntohs(server_addr.sin_port); // port number is an unsigned short
    printf("[MAIN] Start session with %s on port %d\n", client_ip, client_port);

    PacketHeader packetHeader;
    packetHeader.type = GetId;
    IdPacket * idPacket = malloc(sizeof(IdPacket));
    idPacket->id = -1;
    idPacket->header = packetHeader;
    int buf_size = Packet_serialize(buf, &(idPacket->header));

    while((ret = send(socket_desc, buf, buf_size, 0)) < 0) {
        if (errno == EINTR) continue;
        ERROR_HELPER(-1, "[MAIN] Cannot write to socket");
    }
    printf("[MAIN] ID request sent\n");

    //Read id from server
    while((recieved_bytes = recv(socket_desc, buf, sizeof(buf), 0)) < 0)
    {
        if (errno == EINTR)
            continue;
        ERROR_HELPER(-1, "Cannot read from socket");
    }
    
    IdPacket* deserialized_packet = (IdPacket*)Packet_deserialize(buf, sizeof(buf));
    printf("[MAIN] Recived ID = %d.\n", deserialized_packet->id);
    my_id = deserialized_packet->id;
    id = my_id;

    //Send texture to server
    packetHeader.type = PostTexture;
    ImagePacket * imagePacket = malloc(sizeof(ImagePacket));
    imagePacket->id = my_id;
    imagePacket->image = my_texture;
    imagePacket->header = packetHeader;
    buf_size = Packet_serialize(buf, &(imagePacket->header));
    
    while((ret = send(socket_desc, buf, buf_size, 0)) < 0) {
        if (errno == EINTR) continue;
        ERROR_HELPER(-1, "Cannot write to socket");
    }
    
    printf("[MAIN] Texture sent to server\n");

    ret = sem_init(&world_started_sem, NULL, 0);
    ERROR_HELPER(ret, "[MAIN] Error sem init");

    //Reciving SurfaceTexture
    recv_packet_TCP(socket_desc, buf);
    printf("[MAIN] Surface texture recived\n");
    imagePacket = (ImagePacket *) Packet_deserialize(buf, ((PacketHeader *) buf)->size);
    Image * SurfaceTexture = imagePacket->image;


    //Reciving ElevationTexture
    recv_packet_TCP(socket_desc, buf);
    imagePacket = (ImagePacket *) Packet_deserialize(buf, ((PacketHeader *) buf)->size);
    Image * ElevationTexture = imagePacket->image;
    printf("[MAIN] Elevation texture recived\n");

    struct sockaddr_in si_other, si_me; 
    int slen=sizeof(si_other);
    s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ERROR_HELPER(s, "[MAIN] Error socket");

    memset((char *) &si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(SERVER_PORT);
    ret = inet_aton(SERVER_ADDRESS , &si_other.sin_addr);
    ERROR_HELPER(ret, "[MAIN] Error");

    players = players_list_new();

    pthread_t tr;
    pthread_create(&tr, NULL, reciver, (void *) &si_other);
    pthread_detach(tr);

    pthread_t npl;
    pthread_create(&npl, NULL, new_player_listener, &socket_desc);
    pthread_detach(npl);

    pthread_t ust;
    pthread_create(&ust, NULL, update_sender, (void *) &si_other);
    pthread_detach(ust);


    //RUN WORLD
    //Construct the world
    World_init(&world, SurfaceTexture, ElevationTexture,  0.5, 0.5, 0.5);
    //Create my vehicle
    vehicle=(Vehicle*) malloc(sizeof(Vehicle));
    Vehicle_init(vehicle, &world, my_id, my_texture);
    // add it to the world
    World_addVehicle(&world, vehicle);

    ret = sem_post(&world_started_sem);    
    ret = sem_post(&world_started_sem);
    ret = sem_post(&world_started_sem);
    
    ERROR_HELPER(ret, "[MAIN] Error world started");

    WorldViewer_runGlobal(&world, vehicle, &argc, argv);

    return 0;
}
