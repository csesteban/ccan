/* This extract info from _info.c and create json file and also optionally store to db */
#include "infotojson.h"

/* Is A == B ? */
#define streq(a,b) (strcmp((a),(b)) == 0)

/* Does A start with B ? */
#define strstarts(a,b) (strncmp((a),(b),strlen(b)) == 0)

/* This version adds one byte (for nul term) */
static void *grab_file(void *ctx, const char *filename)
{
	unsigned int max = 16384, size = 0;
	int ret, fd;
	char *buffer;

	if (streq(filename, "-"))
		fd = dup(STDIN_FILENO);
	else
		fd = open(filename, O_RDONLY, 0);

	if (fd < 0)
		return NULL;

	buffer = talloc_array(ctx, char, max+1);
	while ((ret = read(fd, buffer + size, max - size)) > 0) {
		size += ret;
		if (size == max)
			buffer = talloc_realloc(ctx, buffer, char, max*=2 + 1);
	}
	if (ret < 0) {
		talloc_free(buffer);
		buffer = NULL;
	} else
		buffer[size] = '\0';
	close(fd);
	return buffer;
}

/* This is a dumb one which copies.  We could mangle instead. */
static char **split(const char *text)
{
	char **lines = NULL;
	unsigned int max = 64, num = 0;

	lines = talloc_array(text, char *, max+1);

	while (*text != '\0') {
		unsigned int len = strcspn(text, "\n");
		lines[num] = talloc_array(lines, char, len + 1);
		memcpy(lines[num], text, len);
		lines[num][len] = '\0';
		text += len + 1;
		if (++num == max)
			lines = talloc_realloc(text, lines, char *, max*=2 + 1);
	}
	lines[num] = NULL;
	return lines;
}

/*combin desc into an array to write to db*/
static char *combinedesc(char **desc)
{
	unsigned int i = 0, size = 0;;
	char *combine;
	
	for(i = 0; desc[i]; i++)
		size += strlen(desc[i]);

	combine = (char *)palloc((size + i)* sizeof(char));
	strcpy(combine, desc[0]);

	for(i = 1; desc[i]; i++) {
		strcat(combine, "\n");
		strcat(combine, desc[i]);
	}
	strreplace(combine,'\'',' ');
	return combine;
}

/*creating json structure for storing to file/db*/
struct json * createjson(char **infofile, char *author)
{
	struct json *jsonobj;
	unsigned int modulename;

	if(infofile == NULL || author == NULL) {
		printf("Error Author or Info file is NULL\n");
		exit(1);
	}

	jsonobj = (struct json *)palloc(sizeof(struct json));

	jsonobj->author = author;

	modulename =  strchr(infofile[0], '-') - infofile[0];
	jsonobj->module = (char *)palloc(sizeof(char) * (modulename - 1));
	strncpy(jsonobj->module, infofile[0], modulename - 1);
	jsonobj->module[modulename - 1] = '\0';

	jsonobj->title = infofile[0];
	jsonobj->desc = &infofile[1];
	
	return jsonobj;
}

/*extracting title and description from _info.c files*/
char **extractinfo(char **file)
{
	char **infofile = NULL;
	unsigned int count = 0, j = 0, size = 0;
	bool printing = false;
	
	while(file[size++]);
	infofile = (char **) palloc(size * sizeof(char *));
	
	for (j = 0; j < size - 1; j++) {
		if (streq(file[j], "/**")) {
			printing = true;
		} 
		else if (streq(file[j], " */"))
			printing = false;
		else if (printing) {
			if (strstarts(file[j], " * "))
				infofile[count++] = file[j] + 3;
			else if (strstarts(file[j], " *"))
				infofile[count++] = file[j] + 2;
			else {
				printf("Error in comments structure\n%d",j);
				exit(1);
			}
		}
	}
	infofile[count] = NULL;
	return infofile;	
}

/*storing json structure to json file*/
int storejsontofile(struct json *jsonobj, char *file)
{
	FILE *fp;
	unsigned int j = 0;
	fp = fopen(file, "wt");
	
	fprintf(fp,"\"Module\":\"%s\",\n",jsonobj->module);
	fprintf(fp,"\"Title\":\"%s\",\n",jsonobj->title);
	fprintf(fp,"\"Author\":\"%s\",\n",jsonobj->author);
	fprintf(fp,"\"Description\":[\n");	
	while(jsonobj->desc[j++])
		fprintf(fp,"{\n\"str\":\"%s\"\n},\n",jsonobj->desc[j - 1]);
	fprintf(fp,"]\n");
	fclose(fp);
	return 1;
	
}

/*storing json structure to db*/
int storejsontodb(struct json *jsonobj, char *db)
{
	char *cmd, *query;
	sqlite3 *handle;
	char *errstr;
	struct db_query *q;
	
	handle = db_open(db);
	
	query = aprintf("SELECT module from search where module=\"%s\";", jsonobj->module);
	q = db_query(handle, query);
	if (!q->num_rows)
		cmd = aprintf("INSERT INTO search VALUES(\"%s\",\"%s\",\"%s\",'%s\');",
			jsonobj->module, jsonobj->author, jsonobj->title, combinedesc(jsonobj->desc));
	else
		cmd = aprintf("UPDATE search set author=\"%s\", title=\"%s\", desc='%s\' where module=\"%s\";",
			jsonobj->author, jsonobj->title, combinedesc(jsonobj->desc), jsonobj->module);
	
	db_command(handle, cmd);	
	db_close(handle);
	return 1;
}

int main(int argc, char *argv[])
{
	char *file;
	char **lines;
	char **infofile;
	
	struct json *jsonobj = NULL;
	
	if(argc < 4) {
		printf("usage: infotojson infofile jsonfile author sqlitedb\n");
		return 1;
	}
		
	file = grab_file(NULL, argv[1]);
	if (!file)
		err(1, "Reading file %s", argv[1]);

	lines = split(file);		
	
	//extract info from lines
	infofile = extractinfo(lines);
	
	//create json obj
	jsonobj = createjson(infofile, argv[3]);
	
	//store to file
	storejsontofile(jsonobj, argv[2]);
	
	if(argv[4] != NULL)
		storejsontodb(jsonobj, argv[4]);
		
	talloc_free(file);
	return 0;
}