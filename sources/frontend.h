#ifndef OD_FRONTEND_H
#define OD_FRONTEND_H

/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
*/

int  od_frontend_errorf(od_client_t*, char*, char*, ...);
int  od_frontend_error(od_client_t*, char*, char*, ...);
void od_frontend(void*);

#endif /* OD_FRONTEND_H */
