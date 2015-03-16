/**
 * getter.c
 *
 * Toke Høiland-Jørgensen
 * 2014-05-07
 */

#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include "getter.h"
#include "worker.h"
#include "util.h"

static size_t get_urls(struct worker *w, char **urls, char *urls_loc, int *total_bytes)
{
	char buf[PIPE_BUF+1] = {}, outbuf[PIPE_BUF+1] = {};
	int len, i;
	size_t bytes, urls_c;
	len = sprintf(outbuf, "URLLIST %s", urls_loc);
	msg_write(w->pipe_w, outbuf, len);
	if((len = msg_read(w->pipe_r, buf, sizeof(buf))) == -1) {
		return 0;
	}
	if(sscanf(buf, "OK %lu bytes %lu urls", &bytes, &urls_c)) {
		*total_bytes += bytes;
		if(!urls_c) return 0;
		for(i = 0; i < urls_c; i++) {
			if((len = msg_read(w->pipe_r, buf, sizeof(buf))) == -1) {
				continue;
			}
			buf[len] = '\0';
			urls[i] = malloc(len+1);
			if(urls[i] == NULL) {
				perror("malloc");
				return i;
			}
			memcpy(urls[i], buf, len+1);
		}
	}
	return urls_c;
}

int get_once(struct worker *workers, char **urls_opt, size_t urls_l, char *urls_loc, int *requests)
{
	struct worker *w;
	int cururl = 0, total_bytes = 0, urls_alloc = 0, reqs = 0, err = 0, bytes, nfds, retval, len, i;
	char **urls = urls_opt;
	fd_set rfds;
	char buf[PIPE_BUF+1] = {}, outbuf[PIPE_BUF+1] = {};
	for(w = workers; w; w = w->next) {
		msg_write(w->pipe_w, "RESET", sizeof("RESET"));
		msg_read(w->pipe_r, buf, sizeof(buf));
		w->status = STATUS_READY;
	}

	if(urls_loc != NULL) {
		urls = malloc(MAX_URLS * sizeof(urls));
		urls_l = get_urls(workers, urls, urls_loc, &total_bytes);
		urls_alloc = 1;
		reqs++;
	}

	do {
		FD_ZERO(&rfds);
		nfds = -1;
		for(w = workers; w; w = w->next) {
			if(w->status == STATUS_READY && cururl < urls_l) {
				w->url = urls[cururl++];
				len = sprintf(outbuf, "URL %s", w->url);
				msg_write(w->pipe_w, outbuf, len);
				w->status = STATUS_WORKING;
			}
			if(w->status == STATUS_WORKING) {
				FD_SET(w->pipe_r, &rfds);
				nfds = max(nfds, w->pipe_r);
			}
		}
		if(nfds == -1) break;
		nfds++;
		retval = select(nfds, &rfds, NULL, NULL, NULL);
		if(retval == -1) {
			perror("select()");
			return -1;
		}
		for(w = workers; w; w = w->next) {
			if(FD_ISSET(w->pipe_r, &rfds)) {
				if((len = msg_read(w->pipe_r, buf, sizeof(buf))) == -1) {
					continue;
				}
				buf[len] = '\0';
				if(sscanf(buf, "OK %d bytes", &bytes) == 1) {
					total_bytes += bytes;
					reqs++;
				} else if(sscanf(buf, "ERR %d", &err) == 1) {
					fprintf(stderr, "cURL error: %s for URL '%s'.\n", curl_easy_strerror(err), w->url);
				}
				w->status = STATUS_READY;
			}
		}
	} while(1);

	if(urls_alloc) {
		for(i = 0; i < urls_l; i++) {
			free(urls[i]);
		}
	}
	*requests = reqs;
	return err ? -err : total_bytes;
}

static void schedule_next(int interval, struct timeval *now, struct timeval *next)
{
	int delay = interval * 1000;
	next->tv_sec = now->tv_sec;
	next->tv_usec = now->tv_usec + delay;
	if(next->tv_usec > 1000000) {
		next->tv_sec += next->tv_usec / 1000000;
		next->tv_usec %= 1000000;
	}
}


static struct worker *workers = NULL;

void kill_workers()
{
	struct worker *w;
	for(w = workers; w; w = w->next) kill_worker(w);
}


int get_loop(struct options *opt)
{
	struct timeval start, end, stop, next;
	double time;
	struct worker *w;
	int i, bytes;
	int count = 0, requests = 0, err = -1;
	for(i = 0; i < opt->workers; i++) {
		w = malloc(sizeof(*w));
		if(w == NULL) {
			perror("malloc");
			exit(EXIT_FAILURE);
		}
		if(start_worker(w, opt) != 0) exit(EXIT_FAILURE);
		w->next = workers;
		workers = w;
	}
	gettimeofday(&stop, NULL);
	start.tv_sec = next.tv_sec = stop.tv_sec;
	start.tv_usec = next.tv_usec = stop.tv_usec;
	stop.tv_sec += opt->run_length;

	do {
		gettimeofday(&start, NULL);
		while(start.tv_sec < next.tv_sec || (start.tv_sec == next.tv_sec && start.tv_usec < next.tv_usec)) {
			if(next.tv_usec - start.tv_usec > USLEEP_THRESHOLD)
				usleep(USLEEP_THRESHOLD);
			gettimeofday(&start, NULL);
		}
		schedule_next(opt->interval, &start, &next);
		bytes = get_once(workers, opt->urls, opt->urls_l, opt->urls_loc, &requests);
		gettimeofday(&end, NULL);
		count++;
		if(bytes < 0) {
			if(err < 0) err = -bytes;
		} else {
			time = end.tv_sec - start.tv_sec;
			time += (double)(end.tv_usec - start.tv_usec) / 1000000;
			fprintf(opt->output, "[%lu.%06lu] %d requests(s) received %lu bytes in %f seconds.\n", (long)end.tv_sec, (long)end.tv_usec, requests, (long)bytes, time);
			err = 0;
		}
	} while((opt->count == 0 || count < opt->count) &&
		(opt->run_length == 0 || end.tv_sec < stop.tv_sec || (end.tv_sec == stop.tv_sec && end.tv_usec < stop.tv_usec)));
	kill_workers();
	return err;
}
